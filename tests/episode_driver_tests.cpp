/// @file episode_driver_tests.cpp
/// @brief Covers EpisodeDriver against a fake episode with hand-driven time:
/// finishing on its own, a timeout whose cancellation finishes within the
/// grace period, a stalled cancellation, and a stop request (a signal) that
/// waits for the episode however long it takes.

#include "agent/episode_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

namespace {

using namespace std::chrono_literals;
using pigpen::agent::DriveOutcome;
using pigpen::agent::EpisodeDriver;
using pigpen::agent::PumpStats;

/// @brief Finishes on a chosen pump, or a chosen number of pumps after the
/// first stop(), and records what the driver asked of it.
class FakeEpisode final : public pigpen::agent::IDrivableEpisode {
public:
  [[nodiscard]] PumpStats pump() override {
    ++pumps;
    if (stops > 0) {
      ++pumps_since_stop;
    }
    if ((finish_on_pump && pumps >= *finish_on_pump) ||
        (finish_pumps_after_stop &&
         pumps_since_stop >= *finish_pumps_after_stop)) {
      done = true;
    }
    return stats;
  }

  [[nodiscard]] bool finished() const override { return done; }

  bool stop() override {
    if (stops == 0) {
      stopped_before_pump = pumps;
    }
    ++stops;
    return true;
  }

  std::optional<int> finish_on_pump{};
  std::optional<int> finish_pumps_after_stop{};
  PumpStats stats{};
  int pumps{};
  int pumps_since_stop{};
  int stops{};
  std::optional<int> stopped_before_pump{};
  bool done{};
};

constexpr EpisodeDriver::Clock::time_point start{1'000s};
constexpr auto timeout = 10s;

} // namespace

TEST_CASE("an episode that finishes on its own ends the drive as finished") {
  FakeEpisode episode;
  episode.finish_on_pump = 3;
  int pumped = 0;
  int timeouts = 0;
  EpisodeDriver driver{
      episode,
      timeout,
      {.on_pumped = [&] { ++pumped; }, .on_timeout = [&] { ++timeouts; }}};

  CHECK_FALSE(driver.outcome());
  CHECK_FALSE(driver.step(start, false));
  CHECK_FALSE(driver.step(start + 1s, false));
  CHECK(driver.step(start + 2s, false) == DriveOutcome::finished);
  CHECK(driver.outcome() == DriveOutcome::finished);
  CHECK(pumped == 3);
  CHECK(episode.stops == 0);
  CHECK(timeouts == 0);
  CHECK_FALSE(driver.timed_out());
  CHECK_FALSE(driver.interrupted());

  // An ended drive reports its outcome again without touching the episode.
  CHECK(driver.step(start + 1h, true) == DriveOutcome::finished);
  CHECK(episode.pumps == 3);
  CHECK(episode.stops == 0);
}

TEST_CASE("the deadline counts from the first step, not construction") {
  FakeEpisode episode;
  EpisodeDriver driver{episode, timeout};
  const auto first = start + 1h;
  CHECK_FALSE(driver.step(first, false));
  CHECK_FALSE(driver.step(first + timeout - 1ms, false));
  CHECK_FALSE(driver.timed_out());
  CHECK(episode.stops == 0);
  CHECK_FALSE(driver.step(first + timeout, false));
  CHECK(driver.timed_out());
  CHECK(episode.stops == 1);
}

TEST_CASE("a timeout cancels once and finishes within the grace period") {
  FakeEpisode episode;
  episode.finish_pumps_after_stop = 2;
  int timeouts = 0;
  int stops_at_timeout = -1;
  EpisodeDriver driver{episode, timeout, {.on_timeout = [&] {
                         ++timeouts;
                         stops_at_timeout = episode.stops;
                       }}};

  CHECK_FALSE(driver.step(start, false));
  CHECK_FALSE(driver.step(start + timeout - 1ms, false));
  CHECK(timeouts == 0);

  CHECK_FALSE(driver.step(start + timeout, false));
  CHECK(driver.timed_out());
  CHECK(timeouts == 1);
  // The notification comes first, then the cancellation request.
  CHECK(stops_at_timeout == 0);
  CHECK(episode.stops == 1);

  CHECK_FALSE(driver.step(start + timeout + 1s, false));
  CHECK(driver.step(start + timeout + 2s, false) == DriveOutcome::timed_out);
  CHECK(timeouts == 1);
  CHECK(episode.stops == 1);
  CHECK_FALSE(driver.interrupted());
}

TEST_CASE("cancellation that outlasts the grace period stalls the drive") {
  FakeEpisode episode;
  EpisodeDriver driver{episode, timeout};

  CHECK_FALSE(driver.step(start, false));
  CHECK_FALSE(driver.step(start + timeout, false));
  const auto grace_end = start + timeout + pigpen::agent::cancellation_grace;
  CHECK_FALSE(driver.step(grace_end - 1ms, false));
  CHECK(driver.step(grace_end, false) == DriveOutcome::cancellation_stalled);
  CHECK(driver.timed_out());
  CHECK(episode.stops == 1);
  CHECK_FALSE(episode.done);
  CHECK(pigpen::agent::cancellation_grace == 15s);
}

TEST_CASE("a stop request cancels before pumping and waits without limit") {
  FakeEpisode episode;
  episode.finish_pumps_after_stop = 5;
  int timeouts = 0;
  EpisodeDriver driver{episode, timeout, {.on_timeout = [&] { ++timeouts; }}};

  // Requested before the first pump, as a signal during startup would be.
  CHECK_FALSE(driver.step(start, true));
  CHECK(driver.interrupted());
  CHECK(episode.stopped_before_pump == 0);
  CHECK(episode.stops == 1);

  // The request latches, and repeating it does not stop again.
  CHECK_FALSE(driver.step(start + 1s, false));
  CHECK(episode.stops == 1);

  // The deadline still passes and requests cancellation once more, but
  // neither it nor the grace period ends a requested stop.
  CHECK_FALSE(driver.step(start + timeout, true));
  CHECK(driver.timed_out());
  CHECK(timeouts == 1);
  CHECK(episode.stops == 2);
  CHECK_FALSE(driver.step(start + 1h, true));
  CHECK(driver.step(start + 2h, true) == DriveOutcome::interrupted);
  CHECK(episode.stops == 2);
}

TEST_CASE("a stop request during the grace period also waits for the end") {
  FakeEpisode episode;
  EpisodeDriver driver{episode, timeout};

  CHECK_FALSE(driver.step(start, false));
  CHECK_FALSE(driver.step(start + timeout, false));
  CHECK(episode.stops == 1);
  CHECK_FALSE(driver.step(start + timeout + 1s, true));
  CHECK(episode.stops == 2);
  CHECK_FALSE(
      driver.step(start + timeout + pigpen::agent::cancellation_grace, true));
  CHECK_FALSE(driver.step(start + 1h, false));
  episode.done = true;
  CHECK(driver.step(start + 2h, false) == DriveOutcome::interrupted);
}

TEST_CASE("a stop request wins over an episode finishing in the same pass") {
  FakeEpisode episode;
  episode.finish_on_pump = 1;
  EpisodeDriver driver{episode, timeout};
  CHECK(driver.step(start, true) == DriveOutcome::interrupted);
  CHECK(episode.stops == 1);
}

TEST_CASE("idle reflects the last pump") {
  FakeEpisode episode;
  EpisodeDriver driver{episode, timeout};
  CHECK(driver.idle());
  episode.stats = {.callbacks_delivered = 1, .events_remaining = 0};
  CHECK_FALSE(driver.step(start, false));
  CHECK_FALSE(driver.idle());
  episode.stats = {.callbacks_delivered = 0, .events_remaining = 2};
  CHECK_FALSE(driver.step(start, false));
  CHECK_FALSE(driver.idle());
  episode.stats = {};
  CHECK_FALSE(driver.step(start, false));
  CHECK(driver.idle());
}

TEST_CASE("drive outcomes have stable names") {
  CHECK(pigpen::agent::drive_outcome_name(DriveOutcome::finished) ==
        "finished");
  CHECK(pigpen::agent::drive_outcome_name(DriveOutcome::timed_out) ==
        "timed_out");
  CHECK(pigpen::agent::drive_outcome_name(DriveOutcome::cancellation_stalled) ==
        "cancellation_stalled");
  CHECK(pigpen::agent::drive_outcome_name(DriveOutcome::interrupted) ==
        "interrupted");
}
