/// @file headless_main.cpp
/// @brief CLI entry point: run one bounded episode and exit.
///
/// Everything here is argv parsing, SIGINT/SIGTERM handling, incremental
/// printing of the transcript and activity feed, and the exit-code policy
/// from docs/running.md. Episode behavior itself lives in agent::Session.
#include "agent/episode_runner.hpp"
#include "agent/session.hpp"
#include "app/headless_options.hpp"
#include "text/catalog.hpp"

#include <chrono>
#include <csignal>
#include <format>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int usage_error_exit = 2;
constexpr int runtime_error_exit = 1;
constexpr int timeout_exit = 3;
constexpr int metrics_error_exit = 4;
constexpr int no_tools_exit = 5;
constexpr int signal_exit_base = 128;

/// A signal handler may not call into Session, iostreams, allocation, or the
/// runtime. Assignment to volatile sig_atomic_t is the only work performed in
/// signal context; ordinary main-loop code observes it and initiates the
/// cooperative cancellation.
volatile std::sig_atomic_t requested_termination_signal = 0;

/// @brief The SIGINT/SIGTERM handler; records the signal number, nothing
/// else.
extern "C" void request_termination(const int signal_number) noexcept {
  requested_termination_signal = signal_number;
}

/// @brief Routes SIGINT and SIGTERM to request_termination().
[[nodiscard]] bool install_signal_handlers() noexcept {
  return std::signal(SIGINT, request_termination) != SIG_ERR &&
         std::signal(SIGTERM, request_termination) != SIG_ERR;
}

/// @brief Signal observed so far, or 0; polled by the pump loop.
[[nodiscard]] int pending_termination_signal() noexcept {
  return static_cast<int>(requested_termination_signal);
}

/// @brief How much of the transcript and activity feed has been printed, so
/// each pump iteration emits only what is new.
struct OutputCursor {
  std::vector<std::size_t> transcript_offsets{};
  std::vector<bool> transcript_announced{};
  std::size_t activities_printed{};
};

void print_usage(std::ostream &output, const std::string_view program) {
  static constexpr unsigned char source[] = {
#embed "../../resources/cli.json"
  };
  static constexpr auto catalog = pigpen::text::make_catalog<source>();
  output << std::format(catalog.get("headless_usage"), program);
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

/// @brief Prints transcript and event-feed entries added since the last
/// call: assistant text streams as per-chunk lines, other roles print once
/// complete, and each decoded tool call gets one line.
void print_updates(const pigpen::agent::Session &session,
                   OutputCursor &cursor) {
  const auto &transcript = session.runner().transcript();
  cursor.transcript_offsets.resize(transcript.size());
  cursor.transcript_announced.resize(transcript.size());

  for (std::size_t index = 0; index < transcript.size(); ++index) {
    const auto &entry = transcript[index];
    if (entry.role != pigpen::agent::TranscriptRole::assistant) {
      if (!cursor.transcript_announced[index]) {
        auto &stream = entry.role == pigpen::agent::TranscriptRole::error
                           ? std::cerr
                           : std::cout;
        stream << transcript_role_name(entry.role) << "[turn=" << entry.turn
               << "]: " << entry.text << '\n';
        stream.flush();
        cursor.transcript_announced[index] = true;
        cursor.transcript_offsets[index] = entry.text.size();
      }
      continue;
    }

    const auto emitted = cursor.transcript_offsets[index];
    if (entry.text.size() <= emitted) {
      continue;
    }
    const std::string_view delta{entry.text.data() + emitted,
                                 entry.text.size() - emitted};
    std::cout << "assistant[turn=" << entry.turn << "]: " << delta;
    if (delta.back() != '\n') {
      std::cout << '\n';
    }
    std::cout.flush();
    cursor.transcript_announced[index] = true;
    cursor.transcript_offsets[index] = entry.text.size();
  }

  const auto &activities = session.tool_activities();
  while (cursor.activities_printed < activities.size()) {
    const auto &activity = activities[cursor.activities_printed++];
    std::cout << "tool[turn=" << activity.turn << ",tick=" << activity.tick
              << "] " << pigpen::agent::tool_kind_name(activity.kind)
              << " args=" << activity.arguments_json
              << " result=" << activity.result_json << " position=("
              << activity.before.x << ',' << activity.before.y << ")->("
              << activity.after.x << ',' << activity.after.y << ")\n";
  }
  std::cout.flush();
}

/// @brief Creates the session, pumps it until the episode finishes — honoring
/// signals and the wall-clock timeout via cooperative cancellation so the
/// JSONL footer is still written — and maps the outcome to an exit code.
[[nodiscard]] int run(const pigpen::app::HeadlessOptions &options) {
  auto created = pigpen::agent::Session::create(
      options.config, options.log_directory, options.prompt_variant);
  if (!created) {
    std::cerr << "startup error: " << created.error() << '\n';
    return runtime_error_exit;
  }
  const auto session = std::move(*created);

  std::cout << "session model=" << std::quoted(options.config.model)
            << " base_url=" << std::quoted(options.config.base_url)
            << " seed=" << options.config.seed
            << " turns=" << options.config.turn_budget
            << " max_tool_rounds=" << options.config.max_tool_rounds
            << " max_world_tool_calls_per_turn="
            << pigpen::agent::max_world_tool_calls_per_turn
            << " max_output_tokens=" << options.config.max_output_tokens
            << " temperature=" << options.config.temperature << '\n'
            << "log_path=" << std::quoted(session->metrics_path().string())
            << '\n';
  std::cout.flush();

  if (options.user_input) {
    static_cast<void>(session->queue_user_input(*options.user_input));
  }
  if (!session->play()) {
    std::cerr << "startup error: episode could not enter playing state\n";
    return runtime_error_exit;
  }

  OutputCursor cursor;
  const auto started = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::time_point> stop_started;
  bool timed_out = false;
  bool cancellation_stalled = false;
  int termination_signal = 0;

  while (session->runner().snapshot().state !=
         pigpen::agent::RunState::finished) {
    const auto before_pump = std::chrono::steady_clock::now();
    if (termination_signal == 0) {
      termination_signal = pending_termination_signal();
      if (termination_signal != 0) {
        stop_started = before_pump;
        std::cerr << "received signal " << termination_signal
                  << "; cancelling active turn and finalizing metrics\n";
        static_cast<void>(session->stop());
      }
    }

    const auto pump = session->pump();
    print_updates(*session, cursor);

    if (session->runner().snapshot().state ==
        pigpen::agent::RunState::finished) {
      break;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!timed_out && now - started >= options.timeout) {
      timed_out = true;
      if (!stop_started) {
        stop_started = now;
      }
      std::cerr << "timeout after " << options.timeout.count()
                << " seconds; cancelling active turn\n";
      static_cast<void>(session->stop());
    }

    // Cancellation is cooperative. Keep pumping to receive its terminal
    // callback, but retain a finite escape hatch so this command is always
    // scriptable.
    // Signal shutdown deliberately keeps pumping until scry delivers its
    // cancellation callback. Exiting early here would abandon the JSONL file
    // instead of preserving the promised complete footer.
    if (stop_started && termination_signal == 0 && now - *stop_started >= 15s) {
      cancellation_stalled = true;
      std::cerr
          << "runtime error: cancellation did not finish within 15 seconds\n";
      break;
    }

    if (pump.callbacks_delivered == 0 && pump.events_remaining == 0) {
      std::this_thread::sleep_for(1ms);
    }
  }

  print_updates(*session, cursor);
  const auto snapshot = session->runner().snapshot();
  const auto reason =
      snapshot.finish_reason
          ? pigpen::agent::finish_reason_name(*snapshot.finish_reason)
          : std::string_view{"unfinished"};
  std::cout << "summary finish_reason=" << reason
            << " turns_used=" << snapshot.turns_used
            << " turn_budget=" << snapshot.turn_budget
            << " score=" << session->world().score()
            << " tool_calls=" << session->tool_call_count() << '\n'
            << "log_path=" << std::quoted(session->metrics_path().string())
            << '\n';
  std::cout.flush();

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
  if (cancellation_stalled || !snapshot.finish_reason ||
      *snapshot.finish_reason == pigpen::agent::FinishReason::error ||
      *snapshot.finish_reason == pigpen::agent::FinishReason::cancelled ||
      *snapshot.finish_reason == pigpen::agent::FinishReason::stopped) {
    if (!snapshot.error.empty()) {
      std::cerr << "terminal error: " << snapshot.error << '\n';
    }
    return runtime_error_exit;
  }
  if (session->tool_call_count() == 0) {
    std::cerr << "validation error: model completed without a successfully "
                 "decoded world-tool invocation\n";
    return no_tools_exit;
  }
  return 0;
}

} // namespace

int main(const int argc, char **argv) {
  if (!install_signal_handlers()) {
    std::cerr << "runtime error: could not install SIGINT/SIGTERM handlers\n";
    return runtime_error_exit;
  }
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  auto options = pigpen::app::parse_headless_options(arguments);
  if (!options) {
    std::cerr << "option error: " << options.error() << "\n\n";
    print_usage(std::cerr, argc > 0 ? argv[0] : "pig-pen-headless");
    return usage_error_exit;
  }
  if (options->help) {
    print_usage(std::cout, argc > 0 ? argv[0] : "pig-pen-headless");
    return 0;
  }
  return run(*options);
}
