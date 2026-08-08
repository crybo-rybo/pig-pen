#include "agent/episode_runner.hpp"
#include "agent/session.hpp"

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

// A signal handler may not call into Session, iostreams, allocation, or the
// runtime. Assignment to volatile sig_atomic_t is the only work performed in
// signal context; ordinary main-loop code observes it and initiates the
// cooperative cancellation.
volatile std::sig_atomic_t requested_termination_signal = 0;

extern "C" void request_termination(const int signal_number) noexcept {
  requested_termination_signal = signal_number;
}

[[nodiscard]] bool install_signal_handlers() noexcept {
  return std::signal(SIGINT, request_termination) != SIG_ERR &&
         std::signal(SIGTERM, request_termination) != SIG_ERR;
}

[[nodiscard]] int pending_termination_signal() noexcept {
  return static_cast<int>(requested_termination_signal);
}

struct Options {
  pigpen::agent::Config config{};
  std::filesystem::path log_directory{"logs"};
  std::chrono::seconds timeout{300};
  std::string prompt_variant{"default"};
  std::optional<std::string> user_input{};
  bool help{};
};

struct OutputCursor {
  std::vector<std::size_t> transcript_offsets{};
  std::vector<bool> transcript_announced{};
  std::size_t events_printed{};
};

void print_usage(std::ostream &output, const std::string_view program) {
  output
      << "Usage: " << program << " --model NAME [options]\n\n"
      << "Run a bounded pig-pen episode against an OpenAI-compatible model "
         "server.\n\n"
      << "Options:\n"
      << "  --base-url URL            Model endpoint (default: "
         "http://127.0.0.1:11434/v1)\n"
      << "  --model NAME              Exact model identifier sent to the "
         "server (required)\n"
      << "  --seed INTEGER            Deterministic world seed (default: 0)\n"
      << "  --turns INTEGER           Episode turn budget, 1..10000 (default: "
         "20)\n"
      << "  --max-tool-rounds INTEGER Tool rounds per turn, 1..64 (default: "
         "8)\n"
      << "  --temperature NUMBER     Sampling temperature, 0.0..2.0 (default: "
         "0.0)\n"
      << "  --log-dir PATH            JSONL output directory (default: logs)\n"
      << "  --timeout-seconds INTEGER Overall episode timeout, 1..86400 "
         "(default: 300)\n"
      << "  --hidden-values           Omit item values from the system prompt\n"
      << "  --no-reward-feedback      Hide numeric reward and score from eat "
         "results\n"
      << "  --opaque-look             Report occupied cells as 'something'\n"
      << "  --prompt-variant NAME     Label recorded in the metrics header\n"
      << "  --input TEXT              Human guidance queued for the first "
         "turn\n"
      << "  --help                    Show this help and exit\n\n"
      << "Values may also use --option=value. PIGPEN_API_KEY supplies an "
         "optional "
         "API key.\n\n"
      << "Exit codes: 0 success, 1 runtime error, 2 invalid options, 3 "
         "timeout,\n"
      << "            4 metrics error, 5 model made no tool calls, 130 "
         "SIGINT,\n"
      << "            143 SIGTERM. Signals request graceful cancellation and "
         "log finalization.\n";
}

[[nodiscard]] std::expected<double, std::string>
parse_temperature(const std::string_view value) {
  double parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed,
                      std::chars_format::general);
  if (value.empty() || error != std::errc{} ||
      end != value.data() + value.size() || !std::isfinite(parsed) ||
      parsed < 0.0 || parsed > 2.0) {
    return std::unexpected(
        "--temperature must be a finite number in the range 0.0..2.0");
  }
  return parsed;
}

template <typename Integer>
[[nodiscard]] std::expected<Integer, std::string>
parse_unsigned(const std::string_view value, const std::string_view option,
               const std::uint64_t maximum) {
  static_assert(std::is_integral_v<Integer> && std::is_unsigned_v<Integer>);
  if (value.empty() || value.front() == '-' || value.front() == '+') {
    return std::unexpected(std::string{option} +
                           " requires an unsigned decimal integer");
  }

  std::uint64_t parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed > maximum ||
      parsed >
          static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
    return std::unexpected(std::string{option} + " must be in the range 0.." +
                           std::to_string(maximum));
  }
  return static_cast<Integer>(parsed);
}

[[nodiscard]] std::expected<Options, std::string> parse_options(const int argc,
                                                                char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (!argument.starts_with("--")) {
      return std::unexpected("unexpected positional argument: " +
                             std::string{argument});
    }

    const auto equals = argument.find('=');
    const auto name = argument.substr(0, equals);
    const auto inline_value = equals == std::string_view::npos
                                  ? std::optional<std::string_view>{}
                                  : std::optional{argument.substr(equals + 1)};
    const auto value = [&]() -> std::expected<std::string_view, std::string> {
      if (inline_value) {
        if (inline_value->empty()) {
          return std::unexpected(std::string{name} + " requires a value");
        }
        return *inline_value;
      }
      if (index + 1 >= argc) {
        return std::unexpected(std::string{name} + " requires a value");
      }
      const std::string_view next{argv[index + 1]};
      if (next.starts_with("--")) {
        return std::unexpected(std::string{name} + " requires a value");
      }
      ++index;
      return next;
    };
    const auto reject_inline_value = [&]() -> std::expected<void, std::string> {
      if (inline_value) {
        return std::unexpected(std::string{name} + " does not take a value");
      }
      return {};
    };

    if (name == "--help") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.help = true;
    } else if (name == "--hidden-values") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.config.known_item_values = false;
    } else if (name == "--no-reward-feedback") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.config.reward_feedback = false;
    } else if (name == "--opaque-look") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.config.opaque_look = true;
    } else if (name == "--base-url") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.config.base_url = *parsed;
    } else if (name == "--model") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.config.model = *parsed;
    } else if (name == "--log-dir") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.log_directory = *parsed;
    } else if (name == "--prompt-variant") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.prompt_variant = *parsed;
    } else if (name == "--input") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.user_input = std::string{*parsed};
    } else if (name == "--seed") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::uint64_t>(
          *parsed, name, std::numeric_limits<std::uint64_t>::max());
      if (!number) {
        return std::unexpected(std::move(number.error()));
      }
      options.config.seed = *number;
    } else if (name == "--turns") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::size_t>(*parsed, name, 10'000);
      if (!number || *number == 0) {
        return std::unexpected(number ? "--turns must be in the range 1..10000"
                                      : std::move(number.error()));
      }
      options.config.turn_budget = *number;
    } else if (name == "--max-tool-rounds") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::uint32_t>(*parsed, name, 64);
      if (!number || *number == 0) {
        return std::unexpected(
            number ? "--max-tool-rounds must be in the range 1..64"
                   : std::move(number.error()));
      }
      options.config.max_tool_rounds = *number;
    } else if (name == "--temperature") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto temperature = parse_temperature(*parsed);
      if (!temperature) {
        return std::unexpected(std::move(temperature.error()));
      }
      options.config.temperature = *temperature;
    } else if (name == "--timeout-seconds") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::uint32_t>(*parsed, name, 86'400);
      if (!number || *number == 0) {
        return std::unexpected(
            number ? "--timeout-seconds must be in the range 1..86400"
                   : std::move(number.error()));
      }
      options.timeout = std::chrono::seconds{*number};
    } else {
      return std::unexpected("unknown option: " + std::string{name});
    }
  }

  if (options.help) {
    return options;
  }
  if (options.config.base_url.empty()) {
    return std::unexpected("--base-url cannot be empty");
  }
  if (options.config.model.empty()) {
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

  const auto &events = session.events();
  while (cursor.events_printed < events.size()) {
    const auto &event = events[cursor.events_printed++];
    std::cout << "tool[turn=" << event.turn << ",tick=" << event.tick << "] "
              << event.tool << " args=" << event.arguments.dump()
              << " result=" << event.result.dump() << " position=("
              << event.before.x << ',' << event.before.y << ")->("
              << event.after.x << ',' << event.after.y << ")\n";
  }
  std::cout.flush();
}

[[nodiscard]] int run(const Options &options) {
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
  bool stopped_for_metrics = false;
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
    if (!stopped_for_metrics && !session->metrics_error().empty()) {
      stopped_for_metrics = true;
      stop_started = now;
      std::cerr << "metrics error: " << session->metrics_error()
                << "; stopping episode\n";
      static_cast<void>(session->stop());
    }
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
    std::cerr << "validation error: model completed without calling a tool\n";
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
  auto options = parse_options(argc, argv);
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
