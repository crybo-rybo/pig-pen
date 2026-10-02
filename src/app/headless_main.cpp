/// @file headless_main.cpp
/// @brief CLI entry point: run one bounded episode and exit.
///
/// Everything here is argv parsing, SIGINT/SIGTERM handling, incremental
/// printing of the transcript and activity feed, and the exit-code policy
/// from docs/running.md. Episode behavior itself lives in agent::Session.
#include "agent/session.hpp"
#include "core/episode_runner.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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
constexpr auto cancellation_grace = 15s;

/// Only a volatile sig_atomic_t store is async-signal-safe; the pump loop
/// polls it and starts the cooperative cancellation itself.
volatile std::sig_atomic_t requested_termination_signal = 0;

extern "C" void request_termination(const int signal_number) noexcept {
  requested_termination_signal = signal_number;
}

struct Options {
  pigpen::core::Config config{};
  std::filesystem::path log_directory{"logs"};
  std::chrono::seconds timeout{300};
  std::string prompt_variant{"default"};
  std::optional<std::string> user_input{};
  bool help{};
};

void print_usage(std::ostream &output, const std::string_view program) {
  output << "Usage: " << program << R"( --model NAME [options]

Run a bounded pig-pen episode against an OpenAI-compatible model server.

Options:
  --base-url URL            Model endpoint (default: http://127.0.0.1:11434/v1)
  --model NAME              Exact model identifier sent to the server (required)
  --seed INTEGER            Deterministic world seed (default: 0)
  --turns INTEGER           Episode turn budget, 1..10000 (default: 20)
  --max-tool-rounds INTEGER Tool rounds per turn, 1..64 (default: 8)
  --temperature NUMBER      Sampling temperature, 0.0..2.0 (default: 0.0)
  --sampling-seed INTEGER   Provider sampling seed, 0..4294967295 (default: unset)
  --log-dir PATH            JSONL output directory (default: logs)
  --timeout-seconds INTEGER Overall episode timeout, 1..86400 (default: 300)
  --hidden-values           Omit item values from the system prompt
  --no-reward-feedback      Hide numeric reward and score from eat results
  --opaque-look             Report occupied cells as 'something'
  --prompt-variant NAME     Label recorded in the metrics header
  --input TEXT              Human guidance queued for the first turn
  --help                    Show this help and exit

Values may also use --option=value. PIGPEN_API_KEY supplies an optional API key.

Exit codes: 0 success, 1 runtime error, 2 invalid options, 3 timeout,
            4 metrics error, 5 no decoded world-tool calls, 130 SIGINT,
            143 SIGTERM. Signals request graceful cancellation and log finalization.
)";
}

/// @brief Parses `--option value` and `--option=value`; errors exit 2.
/// @note `--help` skips the required-value checks so it works on its own.
[[nodiscard]] std::expected<Options, std::string> parse_options(const int argc,
                                                                char **argv) {
  using Result = std::expected<void, std::string>;
  Options options;
  auto &config = options.config;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (!argument.starts_with("--")) {
      return std::unexpected("unexpected positional argument: " +
                             std::string{argument});
    }
    const auto equals = argument.find('=');
    const std::string name{argument.substr(0, equals)};
    const auto inline_value = equals == std::string_view::npos
                                  ? std::optional<std::string_view>{}
                                  : argument.substr(equals + 1);

    const auto value = [&]() -> std::expected<std::string_view, std::string> {
      if (inline_value && !inline_value->empty()) {
        return *inline_value;
      }
      if (!inline_value && index + 1 < argc &&
          !std::string_view{argv[index + 1]}.starts_with("--")) {
        return argv[++index];
      }
      return std::unexpected(name + " requires a value");
    };
    const auto flag = [&](bool &target, const bool setting) -> Result {
      if (inline_value) {
        return std::unexpected(name + " does not take a value");
      }
      target = setting;
      return {};
    };
    const auto text = [&](auto &target) -> Result {
      return value().transform(
          [&](const std::string_view parsed) { target = std::string{parsed}; });
    };
    // from_chars rejects signs for unsigned targets, so "-1" cannot wrap.
    const auto integer = [&](auto &target, const std::uint64_t minimum,
                             const std::uint64_t maximum) -> Result {
      const auto parsed = value();
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      std::uint64_t number{};
      const auto *const end = parsed->data() + parsed->size();
      if (const auto [last, error] =
              std::from_chars(parsed->data(), end, number);
          error != std::errc{} || last != end || number < minimum ||
          number > maximum) {
        return std::unexpected(name + " must be in the range " +
                               std::to_string(minimum) + ".." +
                               std::to_string(maximum));
      }
      target = static_cast<std::remove_cvref_t<decltype(target)>>(number);
      return {};
    };
    const auto temperature = [&]() -> Result {
      const auto parsed = value();
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      double number{};
      const auto *const end = parsed->data() + parsed->size();
      if (const auto [last, error] =
              std::from_chars(parsed->data(), end, number);
          error != std::errc{} || last != end || !std::isfinite(number) ||
          number < 0.0 || number > 2.0) {
        return std::unexpected(
            "--temperature must be a finite number in the range 0.0..2.0");
      }
      config.temperature = number;
      return {};
    };

    constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
    Result result;
    if (name == "--help") {
      result = flag(options.help, true);
    } else if (name == "--hidden-values") {
      result = flag(config.known_item_values, false);
    } else if (name == "--no-reward-feedback") {
      result = flag(config.reward_feedback, false);
    } else if (name == "--opaque-look") {
      result = flag(config.opaque_look, true);
    } else if (name == "--base-url") {
      result = text(config.base_url);
    } else if (name == "--model") {
      result = text(config.model);
    } else if (name == "--log-dir") {
      result = text(options.log_directory);
    } else if (name == "--prompt-variant") {
      result = text(options.prompt_variant);
    } else if (name == "--input") {
      result = text(options.user_input);
    } else if (name == "--seed") {
      result =
          integer(config.seed, 0, std::numeric_limits<std::uint64_t>::max());
    } else if (name == "--sampling-seed") {
      result = integer(config.sampling_seed.emplace(), 0, u32_max);
    } else if (name == "--turns") {
      result = integer(config.turn_budget, 1, pigpen::core::turn_budget_limit);
    } else if (name == "--max-tool-rounds") {
      result =
          integer(config.max_tool_rounds, 1, pigpen::core::tool_rounds_limit);
    } else if (name == "--temperature") {
      result = temperature();
    } else if (name == "--timeout-seconds") {
      std::uint32_t seconds{};
      result = integer(seconds, 1, 86'400);
      options.timeout = std::chrono::seconds{seconds};
    } else {
      result = std::unexpected("unknown option: " + name);
    }
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
  }

  if (options.help) {
    return options;
  }
  if (config.base_url.empty()) {
    return std::unexpected("--base-url cannot be empty");
  }
  if (config.model.empty()) {
    return std::unexpected("--model is required");
  }
  if (options.log_directory.empty()) {
    return std::unexpected("--log-dir cannot be empty");
  }
  if (options.prompt_variant.empty()) {
    return std::unexpected("--prompt-variant cannot be empty");
  }
  return options;
}

[[nodiscard]] std::string_view
transcript_role_name(const pigpen::core::TranscriptRole role) noexcept {
  switch (role) {
  case pigpen::core::TranscriptRole::automatic:
    return "automatic";
  case pigpen::core::TranscriptRole::guidance:
    return "guidance";
  case pigpen::core::TranscriptRole::assistant:
    return "assistant";
  case pigpen::core::TranscriptRole::error:
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
    if (entry.role != pigpen::core::TranscriptRole::assistant) {
      if (index >= previous_size) {
        auto &stream = entry.role == pigpen::core::TranscriptRole::error
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
              << "] " << pigpen::core::tool_kind_name(activity.kind)
              << " args=" << activity.arguments_json
              << " result=" << activity.result_json << " position=("
              << activity.before.x << ',' << activity.before.y << ")->("
              << activity.after.x << ',' << activity.after.y << ")\n";
  }
  std::cout.flush();
}

/// @brief Pumps one session to completion and maps the outcome to an exit
/// code.
[[nodiscard]] int run(const Options &options) {
  auto created = pigpen::agent::Session::create(
      options.config, options.log_directory, options.prompt_variant);
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
            << pigpen::core::max_world_tool_calls_per_turn
            << " max_output_tokens=" << config.max_output_tokens
            << " temperature=" << config.temperature << " sampling_seed=";
  if (config.sampling_seed) {
    std::cout << *config.sampling_seed;
  } else {
    std::cout << "unset";
  }
  std::cout << "\nlog_path=" << std::quoted(session->metrics_path().string())
            << std::endl;

  if (options.user_input) {
    static_cast<void>(session->queue_user_input(*options.user_input));
  }
  if (!session->play()) {
    std::cerr << "startup error: episode could not enter playing state\n";
    return runtime_error_exit;
  }

  // Cancellation is cooperative: after stop() we keep pumping until scry
  // delivers the terminal callback, so the JSONL footer is still written. A
  // signal waits for that indefinitely; a timeout allows a finite grace period
  // so the command stays scriptable.
  OutputCursor cursor;
  auto deadline = std::chrono::steady_clock::now() + options.timeout;
  bool timed_out = false;
  int termination_signal = 0;
  for (;;) {
    if (termination_signal == 0 && requested_termination_signal != 0) {
      termination_signal = requested_termination_signal;
      std::cerr << "received signal " << termination_signal
                << "; cancelling active turn and finalizing metrics\n";
      static_cast<void>(session->stop());
    }

    const auto pump = session->pump();
    print_updates(*session, cursor);
    if (session->runner().snapshot().state ==
        pigpen::core::RunState::finished) {
      break;
    }

    if (const auto now = std::chrono::steady_clock::now(); now >= deadline) {
      if (timed_out) {
        if (termination_signal == 0) {
          std::cerr << "timeout: cancellation did not finish within 15 "
                       "seconds\n";
          break;
        }
      } else {
        timed_out = true;
        deadline = now + cancellation_grace;
        std::cerr << "timeout after " << options.timeout.count()
                  << " seconds; cancelling active turn\n";
        static_cast<void>(session->stop());
      }
    }

    if (pump.callbacks_delivered == 0 && pump.events_remaining == 0) {
      std::this_thread::sleep_for(1ms);
    }
  }

  const auto snapshot = session->runner().snapshot();
  std::cout << "summary finish_reason="
            << (snapshot.finish_reason
                    ? pigpen::core::finish_reason_name(*snapshot.finish_reason)
                    : "unfinished")
            << " turns_used=" << snapshot.turns_used
            << " turn_budget=" << snapshot.turn_budget
            << " score=" << session->world().score()
            << " tool_calls=" << session->tool_activities().size() << '\n'
            << "log_path=" << std::quoted(session->metrics_path().string())
            << std::endl;

  if (!session->metrics_error().empty()) {
    std::cerr << "metrics error: " << session->metrics_error() << '\n';
    return metrics_error_exit;
  }
  if (termination_signal != 0) {
    return signal_exit_base + termination_signal;
  }
  if (timed_out) {
    return timeout_exit;
  }
  // Only the timeout path can leave the loop unfinished, so the reason is set.
  using enum pigpen::core::FinishReason;
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
  if (std::signal(SIGINT, request_termination) == SIG_ERR ||
      std::signal(SIGTERM, request_termination) == SIG_ERR) {
    std::cerr << "runtime error: could not install SIGINT/SIGTERM handlers\n";
    return runtime_error_exit;
  }
  const std::string_view program = argc > 0 ? argv[0] : "pig-pen-headless";
  const auto options = parse_options(argc, argv);
  if (!options) {
    std::cerr << "option error: " << options.error() << "\n\n";
    print_usage(std::cerr, program);
    return usage_error_exit;
  }
  if (options->help) {
    print_usage(std::cout, program);
    return 0;
  }
  return run(*options);
}
