/// @file worker_main.cpp
/// @brief RL worker entry point: play seeds × samples episodes (or the jobs
/// a trainer writes to stdin), P at a time, and write one JSONL record per
/// episode plus a batch record to stdout.
///
/// Everything here is the usage text, the worker's own options, where jobs
/// come from, turning a job into a Session, the records, and the exit-code
/// policy from docs/training.md. Scheduling lives in agent::EpisodeBatch, the
/// deadline and cooperative cancellation in agent::EpisodeDriver, and episode
/// behavior in agent::Session, byte for byte the one the other front ends
/// run. Every callback, including record output, runs on this thread.
#include "agent/episode_batch.hpp"
#include "agent/episode_driver.hpp"
#include "agent/episode_summary.hpp"
#include "agent/session.hpp"
#include "agent/session_options.hpp"
#include "agent/summary_json.hpp"
#include "cli/config_options.hpp"
#include "cli/line_reader.hpp"
#include "cli/option_parser.hpp"
#include "cli/termination_signal.hpp"
#include "cli/worker_jobs.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
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

/// @brief The only `--jobs` source: job lines on standard input.
constexpr std::string_view stdin_jobs{"-"};
/// @brief Job lines read ahead of the batch at most; past this the reader
/// stops and the pipe holds the writer back.
constexpr std::size_t queued_job_lines{1024};
/// @brief Job lines one poll of stdin takes at most, so a flood of rejected
/// lines cannot starve the live episodes.
constexpr int job_lines_per_poll{64};

struct Options {
  pigpen::agent::Config config{};
  pigpen::agent::SessionOptions session{};
  std::vector<std::uint64_t> seeds{};
  /// Engaged only when given, so `--jobs -` can refuse it.
  std::optional<std::uint32_t> samples{};
  /// `--jobs`; only `-` is accepted.
  std::optional<std::string> jobs{};
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
               "repeatable, each seed at most once (required without --jobs)",
               [&options](const std::string_view value) {
                 return pigpen::cli::parse_seed_list(value, options.seeds);
               });
  parser.integer("--samples", "INTEGER", options.samples, 1,
                 pigpen::cli::max_worker_samples,
                 std::format("Episodes per world seed, 1..{} (default: 1)",
                             pigpen::cli::max_worker_samples));
  parser.text("--jobs", "-", options.jobs,
              "Read jobs as JSON lines from stdin instead of --seeds, until "
              "end of input");
  parser.integer("--parallel", "INTEGER", options.parallel, 1, 256,
                 std::format("Episodes in flight at once, 1..256 (default: {})",
                             options.parallel));
  parser.text("--rollout-prefix", "TEXT", options.rollout_prefix,
              std::format("Rollout ids are PREFIX/SEED/SAMPLE unless a job "
                          "line names its own (default: {})",
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
       )" << program
         << R"( --model NAME --jobs - [options] < JOBS

Play every world seed in LIST --samples times against an OpenAI-compatible
model server, --parallel episodes at a time, and write one JSON line per
episode to stdout, then one batch line. Every request carries
X-Pigpen-Rollout: PREFIX/SEED/SAMPLE and X-Pigpen-Seed: SEED.

With --jobs -, each stdin line is one job instead, started as soon as a slot
is free: {"seed":N} plus optional "sample" (default 0), "rollout_id"
(default PREFIX/SEED/SAMPLE, unique per stream), and "sampling_seed" (a
number, or null for --sampling-seed). A line that is not a job gets a
job_error line on stdout. The batch ends at end of input.

Options:
)" << option_parser(defaults).help()
         << R"(
Values may also use --option=value. PIGPEN_API_KEY supplies an optional API key.
Records go to stdout, diagnostics to stderr.

Exit codes: 0 every episode valid, 1 a session could not be created (batch
            aborted) or stdout failed, 2 invalid options, 6 at least one
            episode invalid or job line rejected, 130 SIGINT, 143 SIGTERM.
            Signals cancel in-flight episodes cooperatively and still write
            their records.
)";
}

/// @brief The parsed options and the jobs they describe.
struct Plan {
  Options options{};
  /// Absent for `--help` and for `--jobs -`, whose jobs come from stdin.
  std::optional<pigpen::cli::WorkerJobs> jobs{};
};

/// @brief Parses the command line and describes the jobs; errors exit 2.
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
  if (options.jobs) {
    if (*options.jobs != stdin_jobs) {
      return std::unexpected(
          "--jobs accepts only - (job lines on standard input)");
    }
    const std::pair<bool, std::string_view> exclusive[] = {
        {!options.seeds.empty(), "--seeds"},
        {options.samples.has_value(), "--samples"},
        {options.sampling_seed_base.has_value(), "--sampling-seed-base"},
    };
    for (const auto &[given, name] : exclusive) {
      if (given) {
        return std::unexpected("--jobs - cannot be combined with " +
                               std::string{name} +
                               "; every job line names its own seed");
      }
    }
    return plan;
  }
  auto jobs = pigpen::cli::WorkerJobs::create(
      std::move(options.seeds), options.samples.value_or(1),
      options.rollout_prefix, options.sampling_seed_base);
  if (!jobs) {
    return std::unexpected(std::move(jobs.error()));
  }
  plan.jobs = std::move(*jobs);
  return plan;
}

/// @brief stdout as the record stream: one line per record, flushed so a
/// consumer reading the pipe sees each episode as soon as it ends. After
/// the first failed write it writes nothing more.
class RecordOutput final {
public:
  /// @return false when stdout has failed, now or earlier.
  bool write(const std::string &line) {
    if (failed_) {
      return false;
    }
    std::cout << line << '\n' << std::flush;
    if (!std::cout) {
      failed_ = true;
      std::cerr << "output error: could not write records to stdout; "
                   "cancelling in-flight episodes\n";
    }
    return !failed_;
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
  bool failed_{};
};

/// @brief Tallies across the episode records the batch has reported.
struct RecordTally {
  std::size_t written{};
  std::size_t valid{};
  std::size_t invalid{};
  /// Job lines rejected, each with a `job_error` record.
  std::size_t job_errors{};
};

/// @brief Only spaces, tabs, and carriage returns: a line that is skipped.
[[nodiscard]] bool is_blank(const std::string_view line) {
  return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

/// @brief Where the batch's jobs come from: the command line's seeds ×
/// samples, or job lines read from stdin as slots free up.
class JobFeed final {
public:
  /// @brief The command line's jobs, all known up front.
  explicit JobFeed(const pigpen::cli::WorkerJobs &jobs) : jobs_(&jobs) {}

  /// @brief Job lines from stdin, starting a reader thread now.
  JobFeed(std::string rollout_prefix, RecordTally &tally, RecordOutput &output)
      : stream_(std::in_place, std::move(rollout_prefix)), tally_(&tally),
        output_(&output) {
    reader_.emplace(pigpen::cli::standard_input_bytes(),
                    pigpen::cli::max_job_line_bytes, queued_job_lines);
  }

  /// @brief An episode batch over these jobs.
  [[nodiscard]] pigpen::agent::EpisodeBatch
  batch(const std::size_t parallel, const std::chrono::seconds timeout,
        pigpen::agent::EpisodeBatch::Factory factory) {
    if (jobs_ != nullptr) {
      return {jobs_->size(), parallel, timeout, std::move(factory)};
    }
    return {[this](std::size_t) { return poll(); }, parallel, timeout,
            std::move(factory)};
  }

  /// @brief Job @p index, which the batch has just been told is ready.
  [[nodiscard]] pigpen::cli::WorkerJob take(const std::size_t index) {
    if (jobs_ != nullptr) {
      return jobs_->at(index);
    }
    return *std::exchange(ready_, std::nullopt);
  }

private:
  /// @brief Take stdin lines until one is a job, reporting each rejected
  /// line; never blocks.
  [[nodiscard]] pigpen::agent::JobStatus poll() {
    for (int taken = 0; taken < job_lines_per_poll; ++taken) {
      pigpen::cli::InputLine line;
      switch (reader_->poll(line)) {
      case pigpen::cli::LineStatus::waiting:
        return pigpen::agent::JobStatus::pending;
      case pigpen::cli::LineStatus::closed:
        return pigpen::agent::JobStatus::exhausted;
      case pigpen::cli::LineStatus::line:
        break;
      }
      if (line.truncated) {
        reject(line.number, std::format("line is longer than {} bytes",
                                        pigpen::cli::max_job_line_bytes));
        continue;
      }
      if (is_blank(line.text)) {
        continue;
      }
      auto job = stream_->accept(line.text, line.number);
      if (job) {
        ready_ = std::move(*job);
        return pigpen::agent::JobStatus::ready;
      }
      reject(line.number, std::move(job.error()));
    }
    return pigpen::agent::JobStatus::pending;
  }

  /// @brief Count a rejected job line and write its `job_error` record.
  void reject(const std::size_t number, std::string error) {
    std::cerr << "job line " << number << " rejected: " << error << '\n';
    ++tally_->job_errors;
    static_cast<void>(output_->write(
        pigpen::agent::to_json_line(pigpen::agent::JobErrorRecord{
            .line = number, .error = std::move(error)})));
  }

  const pigpen::cli::WorkerJobs *jobs_{};
  std::optional<pigpen::cli::JobStream> stream_{};
  RecordTally *tally_{};
  RecordOutput *output_{};
  std::optional<pigpen::cli::LineReader> reader_{};
  std::optional<pigpen::cli::WorkerJob> ready_{};
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

/// @brief Writes one job's record and its stderr diagnostics.
void report_episode(const pigpen::agent::Session &session,
                    const pigpen::cli::WorkerJob &job,
                    const pigpen::agent::EpisodeEnd &end, RecordTally &tally,
                    RecordOutput &output) {
  const auto record = episode_record(session, job, end);
  const auto line = pigpen::agent::to_json_line(record);
  const auto &reward = record.summary.reward;
  if (!reward.valid) {
    std::cerr << "episode " << job.rollout_id << " invalid ("
              << reward.invalid_reason << ")";
    if (!record.summary.error.empty()) {
      std::cerr << ": " << record.summary.error;
    }
    std::cerr << '\n';
  }
  if (end.outcome == pigpen::agent::DriveOutcome::cancellation_stalled) {
    std::cerr << "episode " << job.rollout_id
              << ": cancellation did not finish within "
              << pigpen::agent::cancellation_grace.count() << " seconds\n";
  }
  if (!session.metrics_error().empty()) {
    std::cerr << "episode " << job.rollout_id
              << " metrics error: " << session.metrics_error() << '\n';
  }
  // Counted only once the record exists, so a throw above counts nothing.
  ++tally.written;
  ++(reward.valid ? tally.valid : tally.invalid);
  static_cast<void>(output.write(line));
}

/// @brief Runs the batch, writes every record, and maps the result to an
/// exit code.
[[nodiscard]] int run(const Plan &plan,
                      const pigpen::cli::TerminationSignal &termination) {
  const auto &options = plan.options;
  const auto api_key = pigpen::cli::api_key_from_environment();
  RecordTally tally;
  RecordOutput output;
  auto feed = plan.jobs ? std::make_unique<JobFeed>(*plan.jobs)
                        : std::make_unique<JobFeed>(options.rollout_prefix,
                                                    tally, output);

  const auto create_job = [&](const std::size_t index)
      -> std::expected<pigpen::agent::BatchEntry, std::string> {
    auto job = feed->take(index);
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
            [&tally, &output, job = std::move(job),
             borrowed](const pigpen::agent::EpisodeEnd &end) {
              report_episode(*borrowed, job, end, tally, output);
            },
    };
  };

  int termination_signal = 0;
  auto batch =
      feed->batch(options.parallel,
                  std::chrono::seconds{options.timeout_seconds}, create_job);
  const auto result = batch.run({
      .now = [] { return std::chrono::steady_clock::now(); },
      .sleep =
          [](const std::chrono::steady_clock::duration pause) {
            std::this_thread::sleep_for(pause);
          },
      // A signal and a failed stdout both stop the batch cooperatively.
      .stop_requested =
          [&] {
            if (termination_signal == 0 &&
                (termination_signal = termination.received()) != 0) {
              std::cerr << "received signal " << termination_signal
                        << "; cancelling in-flight episodes\n";
            }
            return termination_signal != 0 || output.failed();
          },
  });

  int exit_code = all_valid_exit;
  std::string status{"completed"};
  if (result.error) {
    std::cerr << "startup error: " << *result.error << '\n';
    exit_code = runtime_error_exit;
    status = "aborted";
  } else if (output.failed()) {
    exit_code = runtime_error_exit;
  } else if (termination_signal != 0) {
    exit_code = signal_exit_base + termination_signal;
    status = "interrupted";
  } else if (tally.invalid > 0 || tally.job_errors > 0) {
    exit_code = invalid_episode_exit;
  }
  const pigpen::agent::BatchRecord record{
      .status = std::move(status),
      .jobs = result.jobs,
      .episodes = tally.written,
      .valid = tally.valid,
      .invalid = tally.invalid,
      .not_started = result.not_started,
      .job_errors = tally.job_errors,
      .duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          result.duration),
      .error = result.error.value_or(""),
      .exit_code = exit_code,
  };
  if (!output.write(pigpen::agent::to_json_line(record))) {
    exit_code = runtime_error_exit;
  }
  return exit_code;
}

} // namespace

int main(const int argc, char **argv) {
#ifdef SIGPIPE
  // A consumer that closes the pipe must surface as a failed write, which
  // cancels in-flight episodes cooperatively, not as a fatal signal that
  // loses their log footers.
  std::signal(SIGPIPE, SIG_IGN);
#endif
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
  try {
    return run(*plan, termination);
  } catch (const std::exception &error) {
    // Only pumping an episode can get here; the batch turns factory and
    // report exceptions into an abort. Unwinding closes every session.
    std::cerr << "runtime error: " << error.what() << '\n';
    return runtime_error_exit;
  } catch (...) {
    std::cerr << "runtime error: unknown exception\n";
    return runtime_error_exit;
  }
}
