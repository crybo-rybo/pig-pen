/// @file headless_main.cpp
/// @brief CLI entry point: run one bounded episode and exit.
///
/// Everything here is the usage text, the CLI's own options, incremental
/// printing of the transcript and activity feed, and the exit-code policy
/// from docs/running.md. Shared flags and signal handling live in
/// pigpen_cli, the deadline and cancellation grace in agent::EpisodeDriver,
/// and episode behavior in agent::Session.
#include "agent/episode_driver.hpp"
#include "agent/episode_runner.hpp"
#include "agent/episode_summary.hpp"
#include "agent/session.hpp"
#include "agent/session_options.hpp"
#include "cli/config_options.hpp"
#include "cli/option_parser.hpp"
#include "cli/termination_signal.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int usage_error_exit = 2;
constexpr int runtime_error_exit = 1;
constexpr int timeout_exit = 3;
constexpr int metrics_error_exit = 4;
constexpr int no_tools_exit = 5;
constexpr int signal_exit_base = 128;

struct Options {
  pigpen::agent::Config config{};
  pigpen::agent::SessionOptions session{.log_directory = "logs"};
  std::uint32_t timeout_seconds{300};
  std::optional<std::string> user_input{};
  bool help{};
};

/// @brief The shared Config flags, then the four this CLI adds.
[[nodiscard]] pigpen::cli::OptionParser option_parser(Options &options) {
  pigpen::cli::OptionParser parser;
  pigpen::cli::add_config_options(parser, options.config, options.session);
  parser.text("--log-dir", "PATH", options.session.log_directory,
              "JSONL output directory (default: logs)");
  parser.integer("--timeout-seconds", "INTEGER", options.timeout_seconds, 1,
                 86'400,
                 std::format("Overall episode timeout, 1..86400 (default: {})",
                             options.timeout_seconds));
  parser.text("--input", "TEXT", options.user_input,
              "Human guidance queued for the first turn");
  parser.flag("--help", options.help, true, "Show this help and exit");
  return parser;
}

void print_usage(std::ostream &output, const std::string_view program) {
  Options defaults;
  output << "Usage: " << program << R"( --model NAME [options]

Run a bounded pig-pen episode against an OpenAI-compatible model server.

Options:
)" << option_parser(defaults).help()
         << R"(
Values may also use --option=value. PIGPEN_API_KEY supplies an optional API key.

Exit codes: 0 success, 1 runtime error, 2 invalid options, 3 timeout,
            4 metrics error, 5 no decoded world-tool calls, 130 SIGINT,
            143 SIGTERM. Signals request graceful cancellation and log finalization.
)";
}

/// @brief Parses the command line; errors exit 2.
/// @note `--help` skips the required-value checks so it works on its own.
[[nodiscard]] std::expected<Options, std::string>
parse_options(const std::span<const std::string_view> arguments) {
  Options options;
  auto parser = option_parser(options);
  if (auto parsed = parser.parse(arguments); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (options.help) {
    return options;
  }
  if (auto valid =
          pigpen::cli::validate_config_options(options.config, options.session);
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  return options;
}

[[nodiscard]] std::string_view
transcript_role_name(const pigpen::agent::TranscriptRole role) noexcept {
  switch (role) {
  case pigpen::agent::TranscriptRole::automatic:
    return "automatic";
  case pigpen::agent::TranscriptRole::guidance:
    return "guidance";
  case pigpen::agent::TranscriptRole::assistant:
    return "assistant";
  case pigpen::agent::TranscriptRole::error:
    return "error";
  }
  return "unknown";
}

/// @brief How much of the transcript and activity feed has been printed.
struct OutputCursor {
  /// Bytes printed per transcript entry; only assistant entries grow.
  std::vector<std::size_t> transcript_offsets{};
  std::size_t activities_printed{};
};

/// @brief Prints what is new since the last call: assistant text streams one
/// line per delta, other roles are final when appended and print once, and
/// each decoded tool call gets one line.
void print_updates(const pigpen::agent::Session &session,
                   OutputCursor &cursor) {
  const auto &transcript = session.runner().transcript();
  const auto previous_size = cursor.transcript_offsets.size();
  cursor.transcript_offsets.resize(transcript.size());

  for (std::size_t index = 0; index < transcript.size(); ++index) {
    const auto &entry = transcript[index];
    auto &emitted = cursor.transcript_offsets[index];
    if (entry.role != pigpen::agent::TranscriptRole::assistant) {
      if (index >= previous_size) {
        auto &stream = entry.role == pigpen::agent::TranscriptRole::error
                           ? std::cerr
                           : std::cout;
        stream << transcript_role_name(entry.role) << "[turn=" << entry.turn
               << "]: " << entry.text << '\n';
      }
      continue;
    }
    if (entry.text.size() <= emitted) {
      continue;
    }
    const auto delta = std::string_view{entry.text}.substr(emitted);
    std::cout << "assistant[turn=" << entry.turn << "]: " << delta;
    if (delta.back() != '\n') {
      std::cout << '\n';
    }
    emitted = entry.text.size();
  }

  const auto &activities = session.tool_activities();
  for (; cursor.activities_printed < activities.size();
       ++cursor.activities_printed) {
    const auto &activity = activities[cursor.activities_printed];
    std::cout << "tool[turn=" << activity.turn << ",tick=" << activity.tick
              << "] " << pigpen::agent::tool_kind_name(activity.kind)
              << " args=" << activity.arguments_json
              << " result=" << activity.result_json << " position=("
              << activity.before.x << ',' << activity.before.y << ")->("
              << activity.after.x << ',' << activity.after.y << ")\n";
  }
  std::cout.flush();
}

/// @brief Pumps one session to completion and maps the outcome to an exit
/// code.
[[nodiscard]] int run(const Options &options,
                      const pigpen::cli::TerminationSignal &signal) {
  auto session_options = options.session;
  session_options.api_key = pigpen::cli::api_key_from_environment();
  auto created = pigpen::agent::Session::create(options.config,
                                                std::move(session_options));
  if (!created) {
    std::cerr << "startup error: " << created.error() << '\n';
    return runtime_error_exit;
  }
  const auto session = std::move(*created);
  const auto &config = options.config;

  std::cout << "session model=" << std::quoted(config.model)
            << " base_url=" << std::quoted(config.base_url)
            << " seed=" << config.seed << " turns=" << config.turn_budget
            << " max_tool_rounds=" << config.max_tool_rounds
            << " max_world_tool_calls_per_turn="
            << pigpen::agent::max_world_tool_calls_per_turn
            << " max_output_tokens=" << config.max_output_tokens
            << " temperature=" << config.temperature << " sampling_seed=";
  if (config.sampling_seed) {
    std::cout << *config.sampling_seed;
  } else {
    std::cout << "unset";
  }
  // This CLI always logs; the path is empty only if that ever changes.
  const auto log_path = session->metrics_path().value_or("").string();
  std::cout << "\nlog_path=" << std::quoted(log_path) << std::endl;

  if (options.user_input) {
    static_cast<void>(session->queue_user_input(*options.user_input));
  }
  if (!session->play()) {
    std::cerr << "startup error: episode could not enter playing state\n";
    return runtime_error_exit;
  }

  // Cancellation is cooperative: after stop() the driver keeps pumping until
  // scry delivers the terminal callback, so the JSONL footer is still
  // written. A signal waits for that indefinitely; a timeout allows a finite
  // grace period so the command stays scriptable.
  OutputCursor cursor;
  const std::chrono::seconds timeout{options.timeout_seconds};
  pigpen::agent::EpisodeDriver driver{
      *session,
      timeout,
      {
          .on_pumped = [&] { print_updates(*session, cursor); },
          .on_timeout =
              [&] {
                std::cerr << "timeout after " << timeout.count()
                          << " seconds; cancelling active turn\n";
              },
      },
  };
  int termination_signal = 0;
  const auto stop_requested = [&] {
    if (termination_signal == 0 &&
        (termination_signal = signal.received()) != 0) {
      std::cerr << "received signal " << termination_signal
                << "; cancelling active turn and finalizing metrics\n";
    }
    return termination_signal != 0;
  };
  while (!driver.step(std::chrono::steady_clock::now(), stop_requested())) {
    if (driver.idle()) {
      std::this_thread::sleep_for(1ms);
    }
  }
  if (driver.outcome() == pigpen::agent::DriveOutcome::cancellation_stalled) {
    std::cerr << "timeout: cancellation did not finish within "
              << pigpen::agent::cancellation_grace.count() << " seconds\n";
  }

  const auto snapshot = session->runner().snapshot();
  const auto summary =
      pigpen::agent::summarize_episode(*session, session->reward_weights());
  const auto &reward = summary.reward;
  // Ten significant digits keep the line readable; the log has the exact sum.
  std::cout << "summary finish_reason="
            << (snapshot.finish_reason
                    ? pigpen::agent::finish_reason_name(*snapshot.finish_reason)
                    : "unfinished")
            << " turns_used=" << snapshot.turns_used
            << " turn_budget=" << snapshot.turn_budget
            << " score=" << session->world().score()
            << " tool_calls=" << session->tool_activities().size() << " reward="
            << (reward.valid ? std::format("{:.10g}", reward.total)
                             : std::string{"invalid"})
            << '\n'
            << "log_path=" << std::quoted(log_path) << std::endl;

  if (!session->metrics_error().empty()) {
    std::cerr << "metrics error: " << session->metrics_error() << '\n';
    return metrics_error_exit;
  }
  if (termination_signal != 0) {
    return signal_exit_base + termination_signal;
  }
  if (driver.timed_out()) {
    return timeout_exit;
  }
  // Only the timeout path can leave the loop unfinished, so the reason is set.
  using enum pigpen::agent::FinishReason;
  if (const auto reason = *snapshot.finish_reason;
      reason == error || reason == cancelled || reason == stopped) {
    if (!snapshot.error.empty()) {
      std::cerr << "terminal error: " << snapshot.error << '\n';
    }
    return runtime_error_exit;
  }
  if (session->tool_activities().empty()) {
    std::cerr << "validation error: model completed without a successfully "
                 "decoded world-tool invocation\n";
    return no_tools_exit;
  }
  return 0;
}

} // namespace

int main(const int argc, char **argv) {
  pigpen::cli::TerminationSignal signal;
  if (auto installed = signal.install(); !installed) {
    std::cerr << "runtime error: " << installed.error() << '\n';
    return runtime_error_exit;
  }
  const std::string_view program = argc > 0 ? argv[0] : "pig-pen-headless";
  const std::vector<std::string_view> arguments(argv + (argc > 0 ? 1 : 0),
                                                argv + argc);
  const auto options = parse_options(arguments);
  if (!options) {
    std::cerr << "option error: " << options.error() << "\n\n";
    print_usage(std::cerr, program);
    return usage_error_exit;
  }
  if (options->help) {
    print_usage(std::cout, program);
    return 0;
  }
  return run(*options, signal);
}
