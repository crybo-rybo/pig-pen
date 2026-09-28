/// @file episode_batch.hpp
/// @brief Runs a queue of jobs as episodes, up to P at a time, on one thread.
///
/// The batch is the worker's scheduling policy and nothing else: it knows
/// jobs only by index and episodes only as IDrivableEpisode. A factory turns
/// a job into an episode plus the callback that reports it, one
/// EpisodeDriver per live episode supplies the deadline and cooperative
/// cancellation, and every live episode is pumped once per pass,
/// round-robin. Like the driver, it never reads a clock itself; run() takes
/// the clock and the idle sleep as parameters so tests can drive time.
#pragma once

#include "agent/episode_driver.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>

namespace pigpen::agent {

/// @brief How one started job's episode ended.
struct EpisodeEnd {
  /// Zero-based index of the job.
  std::size_t job{};
  DriveOutcome outcome{};
  /// The job's deadline passed and cancellation was requested for it, even
  /// when a later stop request made the outcome DriveOutcome::interrupted.
  bool timed_out{};
};

/// @brief A started job: the episode to drive and how to report it.
struct BatchEntry {
  /// Driven until its drive ends; must not be null.
  std::shared_ptr<IDrivableEpisode> episode{};
  /// Invoked exactly once, on the batch's thread, when the drive ends and
  /// while @ref episode is still alive; the batch then destroys the driver
  /// and releases the entry, episode last.
  std::function<void(const EpisodeEnd &)> on_end{};
};

/// @brief How a batch ended.
struct BatchResult {
  std::size_t jobs{};
  /// Jobs whose factory call succeeded. Each ended, and reported through
  /// its entry's on_end, exactly once.
  std::size_t started{};
  /// jobs - started: jobs still queued when the batch stopped, plus the
  /// one whose factory call failed. None of them is reported.
  std::size_t not_started{};
  /// A stop request was honoured.
  bool stopped{};
  /// Why the batch was aborted, otherwise absent: the factory's message
  /// when a job could not be started, or the message of an exception the
  /// factory or an on_end threw.
  std::optional<std::string> error{};
  /// From the first step's time to the last step's.
  std::chrono::steady_clock::duration duration{};
};

/// @brief How run() reads time, idles, and learns of a stop request.
struct BatchLoop {
  std::function<std::chrono::steady_clock::time_point()> now{};
  /// Called with idle_pause after a pass in which nothing made progress.
  std::function<void(std::chrono::steady_clock::duration)> sleep{};
  /// Polled once per pass; once true, later false values are ignored.
  std::function<bool()> stop_requested{};
};

/// @brief What run() sleeps after a pass with no progress.
inline constexpr std::chrono::milliseconds idle_pause{1};

/// @brief Runs jobs `0 .. job_count - 1` in order, at most `parallel` at a
/// time, each under its own EpisodeDriver.
///
/// Each step() is one pass: honour a new stop request, start queued jobs
/// while a slot is free, then step every live driver once in start order.
/// When a drive ends, the batch calls the entry's on_end and destroys that
/// episode before the pass moves on, so reports come in completion order
/// and a finished episode's resources (its log, its harness) are released
/// before the next job starts.
///
/// A stop request, or a factory failure, stops the batch: no further job
/// starts, and every live driver is stepped with a stop request, which
/// cancels its episode cooperatively and waits for it to finish, so every
/// started job is still reported. Jobs never started are only counted.
///
/// An exception from the factory counts as a factory failure, and one from
/// an on_end aborts the batch the same way; that job counts as reported and
/// its episode is still released, so nothing is reported twice. Exceptions
/// from pumping an episode propagate to the caller.
class EpisodeBatch final {
public:
  using Clock = EpisodeDriver::Clock;
  /// Turns a job index into its episode, or a message explaining why it
  /// could not be created.
  using Factory =
      std::function<std::expected<BatchEntry, std::string>(std::size_t job)>;

  /// @param parallel Live episodes at most; 0 is treated as 1.
  /// @param timeout Each episode's deadline, from its first step.
  EpisodeBatch(std::size_t job_count, std::size_t parallel,
               Clock::duration timeout, Factory factory);

  EpisodeBatch(const EpisodeBatch &) = delete;
  EpisodeBatch &operator=(const EpisodeBatch &) = delete;

  /// @brief Run one pass.
  /// @param now The caller's current time; it should never go backwards.
  /// @param stop_requested Latches: once true, later false values are
  /// ignored.
  /// @return The result once every started job has ended and nothing more
  /// will start; afterwards further calls return it again and do nothing.
  [[nodiscard]] std::optional<BatchResult> step(Clock::time_point now,
                                                bool stop_requested);

  /// @brief step() until the batch ends, sleeping idle_pause after each pass
  /// that made no progress.
  [[nodiscard]] BatchResult run(const BatchLoop &loop);

  /// @brief The last pass started no job, ended no episode, and every
  /// episode it pumped was idle.
  [[nodiscard]] bool idle() const noexcept;
  /// @brief Episodes currently being driven.
  [[nodiscard]] std::size_t live() const noexcept;

private:
  struct Slot {
    Slot(std::size_t job_index, BatchEntry started, Clock::duration timeout);

    std::size_t job;
    BatchEntry entry;
    /// Declared last so it is destroyed before the episode it drives.
    EpisodeDriver driver;
  };

  std::size_t job_count_;
  std::size_t parallel_;
  Clock::duration timeout_;
  Factory factory_;
  std::list<Slot> slots_{};
  std::size_t next_job_{};
  std::size_t started_{};
  std::optional<Clock::time_point> first_step_{};
  std::optional<std::string> error_{};
  std::optional<BatchResult> result_{};
  bool stop_requested_{};
  bool progressed_{};
};

} // namespace pigpen::agent
