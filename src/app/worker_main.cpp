/// @file worker_main.cpp
/// @brief RL worker entry point: play seeds × samples episodes, P at a time,
/// and write one JSONL record per episode plus a batch record to stdout.
///
/// Everything here is the usage text, the worker's own options, turning a
/// job into a Session, the records, and the exit-code policy from
/// docs/training.md. Scheduling lives in agent::EpisodeBatch, the deadline
/// and cooperative cancellation in agent::EpisodeDriver, and episode
/// behavior in agent::Session, byte for byte the one the other front ends
/// run. Every callback, including record output, runs on this thread.
#include "agent/episode_batch.hpp"
#include "agent/episode_driver.hpp"
#include "agent/episode_summary.hpp"
#include "agent/session.hpp"
#include "agent/session_options.hpp"
#include "agent/summary_json.hpp"
#include "cli/config_options.hpp"
#include "cli/option_parser.hpp"
#include "cli/termination_signal.hpp"
#include "cli/worker_jobs.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int all_valid_exit = 0;
constexpr int runtime_error_exit = 1;
constexpr int usage_error_exit = 2;
constexpr int invalid_episode_exit = 6;
constexpr int signal_exit_base = 128;

/// @brief invalid_reason of an episode its worker deadline cut short.
constexpr std::string_view timeout_reason{"timeout"};

struct Options {
  pigpen::agent::Config config{};
  pigpen::agent::SessionOptions session{};
  std::vector<std::uint64_t> seeds{};
  std::uint32_t samples{1};
  std::uint32_t parallel{1};
  std::string rollout_prefix{"rollout"};
  std::optional<std::uint32_t> sampling_seed_base{};
  std::uint32_t timeout_seconds{300};
  bool help{};
};

/// @brief The shared Config flags (world seeds come from --seeds instead),
/// then the worker's own.
[[nodiscard]] pigpen::cli::OptionParser option_parser(Options &options) {
  constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
  pigpen::cli::OptionParser parser;
  pigpen::cli::add_config_options(parser, options.config, options.session,
                                  pigpen::cli::WorldSeedOption::omitted);
  parser.rejected("--seed",
                  "--seed is not accepted by the worker; give world seeds "
                  "with --seeds");
  parser.value("--seeds", "LIST",
               "World seeds: comma-separated seeds and inclusive A-B ranges; "
               "repeatable, each seed at most once (required)",
               [&options](const std::string_view value) {
                 return pigpen::cli::parse_seed_list(value, options.seeds);
               });
  parser.integer("--samples", "INTEGER", options.samples, 1,
                 pigpen::cli::max_worker_samples,
                 std::format("Episodes per world seed, 1..{} (default: {})",
                             pigpen::cli::max_worker_samples, options.samples));
  parser.integer("--parallel", "INTEGER", options.parallel, 1, 256,
                 std::format("Episodes in flight at once, 1..256 (default: {})",
                             options.parallel));
  parser.text("--rollout-prefix", "TEXT", options.rollout_prefix,
              std::format("Rollout ids are PREFIX/SEED/SAMPLE (default: {})",
                          options.rollout_prefix));
  parser.integer("--sampling-seed-base", "INTEGER", options.sampling_seed_base,
                 0, u32_max,
                 "Send sample K with provider sampling seed BASE + K "
                 "(default: unset, server-random)");
  parser.integer("--timeout-seconds", "INTEGER", options.timeout_seconds, 1,
                 86'400,
                 std::format("Per-episode timeout, 1..86400 (default: {})",
                             options.timeout_seconds));
  parser.text("--log-dir", "DIR", options.session.log_directory,
              "Also write each episode's JSONL log here (default: off)");
  parser.value("--header", "NAME=VALUE",
               "Extra request header on every request; repeatable",
               [&options](const std::string_view assignment) {
                 return pigpen::cli::parse_request_header(
                     assignment, options.session.request_headers);
               });
  parser.flag("--help", options.help, true, "Show this help and exit");
  return parser;
}

void print_usage(std::ostream &output, const std::string_view program) {
  Options defaults;
  output << "Usage: " << program << R"( --model NAME --seeds LIST [options]

Play every world seed in LIST --samples times against an OpenAI-compatible
model server, --parallel episodes at a time, and write one JSON line per
episode to stdout, then one batch line. Every request carries
X-Pigpen-Rollout: PREFIX/SEED/SAMPLE and X-Pigpen-Seed: SEED.

Options:
)" << option_parser(defaults).help()
         << R"(
Values may also use --option=value. PIGPEN_API_KEY supplies an optional API key.
Records go to stdout, diagnostics to stderr.

Exit codes: 0 every episode valid, 1 a session could not be created (batch
            aborted), 2 invalid options, 6 at least one episode invalid,
            130 SIGINT, 143 SIGTERM. Signals cancel in-flight episodes
            cooperatively and still write their records.
)";
}

/// @brief The parsed options and the jobs they describe.
struct Plan {
  Options options{};
  std::vector<pigpen::cli::WorkerJob> jobs{};
};

/// @brief Parses the command line and expands the jobs; errors exit 2.
/// @note `--help` skips the required-value checks so it works on its own.
[[nodiscard]] std::expected<Plan, std::string>
parse_plan(const std::span<const std::string_view> arguments) {
  Plan plan;
  auto &options = plan.options;
  auto parser = option_parser(options);
  if (auto parsed = parser.parse(arguments); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (options.help) {
    return plan;
  }
  if (auto valid =
          pigpen::cli::validate_config_options(options.config, options.session);
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  if (auto valid = pigpen::cli::validate_rollout_prefix(options.rollout_prefix);
      !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  if (options.config.sampling_seed && options.sampling_seed_base) {
    return std::unexpected(
        "--sampling-seed and --sampling-seed-base cannot both be given");
  }
  auto jobs = pigpen::cli::expand_jobs(options.seeds, options.samples,
                                       options.rollout_prefix,
                                       options.sampling_seed_base);
  if (!jobs) {
    return std::unexpected(std::move(jobs.error()));
  }
  plan.jobs = std::move(*jobs);
  return plan;
}

/// @brief Writes one record line and flushes it, so a consumer reading the
/// pipe sees each episode as soon as it ends.
/// @return false once stdout has failed.
bool write_record(const std::string &line) {
  std::cout << line << '\n' << std::flush;
  return static_cast<bool>(std::cout);
}

/// @brief Tallies across the episode records the batch has written.
struct RecordTally {
  std::size_t written{};
  std::size_t valid{};
  std::size_t invalid{};
  bool output_failed{};
};

/// @brief The `episode` record of a job whose drive just ended, while its
/// session is still alive.
[[nodiscard]] pigpen::agent::EpisodeRecord
episode_record(const pigpen::agent::Session &session,
               const pigpen::cli::WorkerJob &job,
               const pigpen::agent::EpisodeEnd &end) {
  auto summary =
      pigpen::agent::summarize_episode(session, session.reward_weights());
  // The deadline, not the cancellation it caused, is why the episode ended.
  if (end.timed_out) {
    summary.reward.valid = false;
    summary.reward.invalid_reason = std::string{timeout_reason};
  }
  return {
      .summary = std::move(summary),
      .config = session.config(),
      .sample = job.sample,
  };
}

/// @brief Runs the batch, writes every record, and maps the result to an
/// exit code.
[[nodiscard]] int run(const Plan &plan,
                      const pigpen::cli::TerminationSignal &termination) {
  const auto &options = plan.options;
  const auto api_key = pigpen::cli::api_key_from_environment();
  RecordTally tally;

  const auto create_job = [&](const std::size_t index)
      -> std::expected<pigpen::agent::BatchEntry, std::string> {
    const auto &job = plan.jobs[index];
    auto config = options.config;
    config.seed = job.seed;
    if (job.sampling_seed) {
      config.sampling_seed = job.sampling_seed;
    }
    auto session_options = options.session;
    session_options.api_key = api_key;
    session_options.rollout_id = job.rollout_id;
    session_options.request_headers.emplace_back(
        std::string{pigpen::cli::seed_header_name}, std::to_string(job.seed));

    auto created = pigpen::agent::Session::create(std::move(config),
                                                  std::move(session_options));
    if (!created) {
      return std::unexpected(job.rollout_id + ": " + created.error());
    }
    std::shared_ptr<pigpen::agent::Session> session = std::move(*created);
    if (!session->play()) {
      return std::unexpected(job.rollout_id +
                             ": episode could not enter playing state");
    }
    // The entry owns the session; this callback only borrows it, and the
    // batch calls it before releasing the entry.
    auto *const borrowed = session.get();
    return pigpen::agent::BatchEntry{
        .episode = std::move(session),
        .on_end =
            [&tally, &job, borrowed](const pigpen::agent::EpisodeEnd &end) {
              const auto record = episode_record(*borrowed, job, end);
              const auto &reward = record.summary.reward;
              ++tally.written;
              if (reward.valid) {
                ++tally.valid;
              } else {
                ++tally.invalid;
                std::cerr << "episode " << job.rollout_id << " invalid ("
                          << reward.invalid_reason << ")";
                if (!record.summary.error.empty()) {
                  std::cerr << ": " << record.summary.error;
                }
                std::cerr << '\n';
              }
              if (end.outcome ==
                  pigpen::agent::DriveOutcome::cancellation_stalled) {
                std::cerr << "episode " << job.rollout_id
                          << ": cancellation did not finish within "
                          << pigpen::agent::cancellation_grace.count()
                          << " seconds\n";
              }
              if (!borrowed->metrics_error().empty()) {
                std::cerr << "episode " << job.rollout_id
                          << " metrics error: " << borrowed->metrics_error()
                          << '\n';
              }
              if (!write_record(pigpen::agent::to_json_line(record))) {
                tally.output_failed = true;
              }
            },
    };
  };

  int termination_signal = 0;
  pigpen::agent::EpisodeBatch batch{
      plan.jobs.size(), options.parallel,
      std::chrono::seconds{options.timeout_seconds}, create_job};
  const auto result = batch.run({
      .now = [] { return std::chrono::steady_clock::now(); },
      .sleep =
          [](const std::chrono::steady_clock::duration pause) {
            std::this_thread::sleep_for(pause);
          },
      .stop_requested =
          [&] {
            if (termination_signal == 0 &&
                (termination_signal = termination.received()) != 0) {
              std::cerr << "received signal " << termination_signal
                        << "; cancelling in-flight episodes\n";
            }
            return termination_signal != 0;
          },
  });

  int exit_code = all_valid_exit;
  std::string status{"completed"};
  if (result.error) {
    std::cerr << "startup error: " << *result.error << '\n';
    exit_code = runtime_error_exit;
    status = "aborted";
  } else if (termination_signal != 0) {
    exit_code = signal_exit_base + termination_signal;
    status = "interrupted";
  } else if (tally.output_failed) {
    exit_code = runtime_error_exit;
  } else if (tally.invalid > 0) {
    exit_code = invalid_episode_exit;
  }
  const pigpen::agent::BatchRecord record{
      .status = std::move(status),
      .jobs = result.jobs,
      .episodes = tally.written,
      .valid = tally.valid,
      .invalid = tally.invalid,
      .not_started = result.not_started,
      .duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          result.duration),
      .error = result.error.value_or(""),
      .exit_code = exit_code,
  };
  if (!write_record(pigpen::agent::to_json_line(record)) ||
      tally.output_failed) {
    std::cerr << "output error: could not write records to stdout\n";
    if (exit_code == all_valid_exit || exit_code == invalid_episode_exit) {
      exit_code = runtime_error_exit;
    }
  }
  return exit_code;
}

} // namespace

int main(const int argc, char **argv) {
  // Deliberately never destroyed: restoring the default handlers after main
  // returns would let a late signal replace the exit code already computed.
  auto &termination = *new pigpen::cli::TerminationSignal{};
  if (auto installed = termination.install(); !installed) {
    std::cerr << "runtime error: " << installed.error() << '\n';
    return runtime_error_exit;
  }
  const std::string_view program = argc > 0 ? argv[0] : "pig-pen-worker";
  const std::vector<std::string_view> arguments(argv + (argc > 0 ? 1 : 0),
                                                argv + argc);
  const auto plan = parse_plan(arguments);
  if (!plan) {
    std::cerr << "option error: " << plan.error() << "\n\n";
    print_usage(std::cerr, program);
    return usage_error_exit;
  }
  if (plan->options.help) {
    print_usage(std::cout, program);
    return 0;
  }
  return run(*plan, termination);
}
