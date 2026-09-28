/// @file episode_driver.hpp
/// @brief Drives one episode to its end under a deadline, cooperatively.
///
/// The driver is the one copy of the "pump until finished" policy: an
/// overall deadline, cooperative cancellation when it passes, a fixed grace
/// period for that cancellation, and an external stop request (a signal)
/// that waits for cancellation indefinitely. It never sleeps and never reads
/// a clock; the caller supplies the time and decides how to idle, so one
/// thread can drive several episodes and tests can drive time by hand.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace pigpen::agent {

/// @brief What one pump() pass accomplished; both front ends surface this.
struct PumpStats {
  std::size_t callbacks_delivered{};
  std::size_t events_remaining{};
};

/// @brief The part of an episode a driver needs. Session implements it.
class IDrivableEpisode {
public:
  virtual ~IDrivableEpisode() = default;

  /// @brief Deliver pending work without blocking.
  [[nodiscard]] virtual PumpStats pump() = 0;
  /// @brief True once the episode has delivered its terminal result.
  [[nodiscard]] virtual bool finished() const = 0;
  /// @brief Request cooperative cancellation; the episode is not finished
  /// until finished() says so.
  /// @return true when the request changed anything.
  virtual bool stop() = 0;
};

/// @brief How a driven episode ended.
enum class DriveOutcome : std::uint8_t {
  /// Finished on its own, before the deadline and without a stop request.
  finished,
  /// The deadline passed and the cancellation it started finished in time.
  timed_out,
  /// The deadline passed and cancellation did not finish within
  /// cancellation_grace; the episode is still unfinished.
  cancellation_stalled,
  /// A stop was requested and the episode then finished; this wins over a
  /// timeout, and a requested stop is waited for without limit.
  interrupted,
};

[[nodiscard]] std::string_view
drive_outcome_name(DriveOutcome outcome) noexcept;

/// @brief How long cancellation started by the deadline may take.
inline constexpr std::chrono::seconds cancellation_grace{15};

/// @brief Optional notifications, invoked from step() on the caller's thread.
struct EpisodeDriverObservers {
  /// After every pump, before the finished check; a CLI prints here.
  std::function<void()> on_pumped{};
  /// Once, when the deadline passes, just before stop() is requested.
  std::function<void()> on_timeout{};
};

/// @brief Pumps one IDrivableEpisode until it finishes, times out and
/// finishes, stalls in cancellation, or finishes after a stop request.
///
/// Each step() is one pass: honour a new stop request, pump, report a
/// finished episode, then check the deadline. The deadline is @p timeout
/// after the first step's @p now. When it passes, the driver requests
/// stop() and allows cancellation_grace more; if that also passes without
/// a stop request, the episode is left unfinished as cancellation_stalled.
/// A stop request calls stop() once and then waits without limit, because
/// finishing is what writes the log footer.
class EpisodeDriver final {
public:
  using Clock = std::chrono::steady_clock;

  /// @param episode Must outlive the driver.
  /// @param timeout Overall deadline, measured from the first step().
  EpisodeDriver(IDrivableEpisode &episode, Clock::duration timeout,
                EpisodeDriverObservers observers = {});

  /// @brief Run one pass.
  /// @param now The caller's current time; it should never go backwards.
  /// @param stop_requested Latches: once true, later false values are
  /// ignored.
  /// @return The outcome once the drive has ended, otherwise nullopt. After
  /// the end, further calls return the same outcome without pumping.
  [[nodiscard]] std::optional<DriveOutcome> step(Clock::time_point now,
                                                 bool stop_requested);

  /// @brief The outcome, once step() has returned one.
  [[nodiscard]] std::optional<DriveOutcome> outcome() const noexcept;
  /// @brief The last pump delivered nothing and left nothing queued; a
  /// caller with nothing else to do may sleep briefly.
  [[nodiscard]] bool idle() const noexcept;
  /// @brief The deadline has passed and stop() was requested for it.
  [[nodiscard]] bool timed_out() const noexcept;
  /// @brief A stop request was seen and stop() was requested for it.
  [[nodiscard]] bool interrupted() const noexcept;

private:
  IDrivableEpisode &episode_;
  Clock::duration timeout_;
  EpisodeDriverObservers observers_;
  std::optional<Clock::time_point> deadline_{};
  PumpStats last_pump_{};
  std::optional<DriveOutcome> outcome_{};
  bool timed_out_{};
  bool interrupted_{};
};

} // namespace pigpen::agent
