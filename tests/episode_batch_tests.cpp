/// @file episode_batch_tests.cpp
/// @brief Covers EpisodeBatch with a fake episode factory and hand-driven
/// time: start order and the parallel cap, reports in completion order and
/// exactly once while the episode is alive, per-episode timeouts (including
/// a stalled cancellation), a stop request cancelling every live episode
/// and starting nothing more, a factory failure aborting the batch the same
/// way, an exception from the factory or a report aborting it without
/// double reports, and run() sleeping only after a pass with no progress.

#include "agent/episode_batch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using pigpen::agent::BatchEntry;
using pigpen::agent::DriveOutcome;
using pigpen::agent::EpisodeBatch;
using pigpen::agent::EpisodeEnd;
using pigpen::agent::PumpStats;

/// @brief How one fake job behaves.
struct Script {
  /// Finish on this pump (1-based), counting from the episode's creation.
  std::optional<int> finish_on_pump{};
  /// Finish this many pumps after the first stop(); absent never finishes
  /// once stopped unless finish_on_pump says so.
  std::optional<int> finish_pumps_after_stop{1};
  /// Reported by every pump.
  PumpStats stats{};
};

/// @brief What the fakes observed, shared by every episode of one batch.
struct Observed {
  int alive{};
  int peak_alive{};
  std::vector<std::size_t> created{};
  std::vector<std::size_t> destroyed{};
  std::map<std::size_t, int> stops{};
};

class FakeEpisode final : public pigpen::agent::IDrivableEpisode {
public:
  FakeEpisode(std::size_t job, Script script, Observed &observed)
      : job_(job), script_(script), observed_(observed) {
    observed_.created.push_back(job_);
    observed_.peak_alive = std::max(observed_.peak_alive, ++observed_.alive);
  }

  ~FakeEpisode() override {
    --observed_.alive;
    observed_.destroyed.push_back(job_);
  }

  FakeEpisode(const FakeEpisode &) = delete;
  FakeEpisode &operator=(const FakeEpisode &) = delete;

  [[nodiscard]] PumpStats pump() override {
    ++pumps_;
    if (stops_ > 0) {
      ++pumps_since_stop_;
    }
    if ((script_.finish_on_pump && pumps_ >= *script_.finish_on_pump) ||
        (stops_ > 0 && script_.finish_pumps_after_stop &&
         pumps_since_stop_ >= *script_.finish_pumps_after_stop)) {
      done_ = true;
    }
    return script_.stats;
  }

  [[nodiscard]] bool finished() const override { return done_; }

  bool stop() override {
    ++stops_;
    ++observed_.stops[job_];
    return true;
  }

private:
  std::size_t job_;
  Script script_;
  Observed &observed_;
  int pumps_{};
  int pumps_since_stop_{};
  int stops_{};
  bool done_{};
};

/// @brief A report as the test records it, with whether the episode was
/// still alive when on_end ran.
struct Report {
  EpisodeEnd end{};
  bool episode_alive{};
};

/// @brief A batch whose factory builds FakeEpisodes from per-job scripts
/// (a missing job uses the default Script) and fails for the jobs in
/// @ref fail.
struct Harness {
  std::map<std::size_t, Script> scripts{};
  std::set<std::size_t> fail{};
  Observed observed{};
  std::vector<Report> reports{};
  std::vector<std::size_t> factory_calls{};

  [[nodiscard]] EpisodeBatch
  batch(std::size_t jobs, std::size_t parallel,
        EpisodeBatch::Clock::duration timeout = 10s) {
    return EpisodeBatch{
        jobs, parallel, timeout,
        [this](
            const std::size_t job) -> std::expected<BatchEntry, std::string> {
          factory_calls.push_back(job);
          if (fail.contains(job)) {
            return std::unexpected("job " + std::to_string(job) + " failed");
          }
          const auto found = scripts.find(job);
          auto episode = std::make_shared<FakeEpisode>(
              job, found == scripts.end() ? Script{} : found->second, observed);
          const auto *const alive = &observed.destroyed;
          return BatchEntry{
              .episode = std::move(episode),
              .on_end =
                  [this, job, alive](const EpisodeEnd &end) {
                    bool destroyed = false;
                    for (const auto gone : *alive) {
                      destroyed = destroyed || gone == job;
                    }
                    reports.push_back(
                        {.end = end, .episode_alive = !destroyed});
                  },
          };
        }};
  }

  [[nodiscard]] std::vector<std::size_t> reported_jobs() const {
    std::vector<std::size_t> jobs;
    for (const auto &report : reports) {
      jobs.push_back(report.end.job);
    }
    return jobs;
  }
};

constexpr EpisodeBatch::Clock::time_point start{1'000s};

} // namespace

TEST_CASE("a batch starts jobs in order under the parallel cap and reports "
          "them in completion order",
          "[batch]") {
  Harness harness;
  // Job 0 is slowest, so later jobs overtake it.
  harness.scripts[0] = {.finish_on_pump = 4};
  harness.scripts[1] = {.finish_on_pump = 1};
  harness.scripts[2] = {.finish_on_pump = 2};
  harness.scripts[3] = {.finish_on_pump = 1};
  harness.scripts[4] = {.finish_on_pump = 1};
  auto batch = harness.batch(5, 2);

  std::optional<pigpen::agent::BatchResult> result;
  auto now = start;
  for (int pass = 0; pass < 20 && !result; ++pass) {
    result = batch.step(now, false);
    CHECK(batch.live() <= 2);
    now += 1ms;
  }
  REQUIRE(result);
  CHECK(harness.observed.peak_alive == 2);
  CHECK(harness.observed.alive == 0);
  CHECK(harness.factory_calls == std::vector<std::size_t>{0, 1, 2, 3, 4});
  CHECK(harness.observed.created == std::vector<std::size_t>{0, 1, 2, 3, 4});
  // Pass 1: 0 and 1 start, 1 ends. Pass 2: 2 starts. Pass 3: 2 ends.
  // Pass 4: 3 starts; 0 then 3 end, in start order within the pass.
  // Pass 5: 4 starts and ends.
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{1, 2, 0, 3, 4});
  // Each episode is reported once, alive, and destroyed right after.
  CHECK(harness.observed.destroyed == harness.reported_jobs());
  for (const auto &report : harness.reports) {
    CHECK(report.episode_alive);
    CHECK(report.end.outcome == DriveOutcome::finished);
    CHECK_FALSE(report.end.timed_out);
  }
  CHECK(harness.observed.stops.empty());
  CHECK(result->jobs == 5);
  CHECK(result->started == 5);
  CHECK(result->not_started == 0);
  CHECK_FALSE(result->stopped);
  CHECK_FALSE(result->error);
  CHECK(result->duration == 4ms);

  // An ended batch returns the same result and does nothing more.
  const auto again = batch.step(now + 1h, true);
  REQUIRE(again);
  CHECK_FALSE(again->stopped);
  CHECK(harness.reports.size() == 5);
  CHECK(harness.factory_calls.size() == 5);
}

TEST_CASE("a batch with no jobs ends on its first step", "[batch]") {
  Harness harness;
  auto batch = harness.batch(0, 3);
  const auto result = batch.step(start, false);
  REQUIRE(result);
  CHECK(result->jobs == 0);
  CHECK(result->started == 0);
  CHECK(harness.factory_calls.empty());
}

TEST_CASE("a parallel cap of zero runs one episode at a time", "[batch]") {
  Harness harness;
  harness.scripts[0] = {.finish_on_pump = 2};
  harness.scripts[1] = {.finish_on_pump = 2};
  auto batch = harness.batch(2, 0);
  auto now = start;
  while (!batch.step(now, false)) {
    CHECK(batch.live() <= 1);
    now += 1ms;
  }
  CHECK(harness.observed.peak_alive == 1);
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{0, 1});
}

TEST_CASE("each episode gets its own deadline from its first step",
          "[batch][timeout]") {
  Harness harness;
  // Job 0 never finishes on its own but honours cancellation; job 1 is
  // quick; job 2 starts late and must not inherit job 0's deadline.
  harness.scripts[0] = {.finish_pumps_after_stop = 1};
  harness.scripts[1] = {.finish_on_pump = 1};
  harness.scripts[2] = {.finish_on_pump = 3};
  auto batch = harness.batch(3, 2, 10s);

  CHECK_FALSE(batch.step(start, false));      // 0, 1 start; 1 ends.
  CHECK_FALSE(batch.step(start + 9s, false)); // 2 starts.
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{1});
  // Job 0's deadline passes: it is asked to stop once.
  CHECK_FALSE(batch.step(start + 10s, false));
  CHECK(harness.observed.stops == std::map<std::size_t, int>{{0, 1}});
  // It finishes on the next pump; job 2 finishes on its third, well
  // within its own deadline.
  const auto result = batch.step(start + 11s, false);
  REQUIRE(result);
  REQUIRE(harness.reports.size() == 3);
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{1, 0, 2});
  CHECK(harness.reports[1].end.outcome == DriveOutcome::timed_out);
  CHECK(harness.reports[1].end.timed_out);
  CHECK(harness.reports[2].end.outcome == DriveOutcome::finished);
  CHECK_FALSE(harness.reports[2].end.timed_out);
  CHECK(harness.observed.stops.size() == 1);
  CHECK_FALSE(result->stopped);
}

TEST_CASE("a stalled cancellation is reported and the batch moves on",
          "[batch][timeout]") {
  Harness harness;
  harness.scripts[0] = {.finish_pumps_after_stop = std::nullopt};
  harness.scripts[1] = {.finish_on_pump = 1};
  auto batch = harness.batch(2, 1, 10s);

  CHECK_FALSE(batch.step(start, false));
  CHECK_FALSE(batch.step(start + 10s, false));
  CHECK(harness.observed.stops.at(0) == 1);
  CHECK_FALSE(
      batch.step(start + 10s + pigpen::agent::cancellation_grace - 1ms, false));
  CHECK(harness.reports.empty());
  // The grace period passes: job 0 is reported unfinished and destroyed.
  CHECK_FALSE(
      batch.step(start + 10s + pigpen::agent::cancellation_grace, false));
  REQUIRE(harness.reports.size() == 1);
  CHECK(harness.reports[0].end.outcome == DriveOutcome::cancellation_stalled);
  CHECK(harness.reports[0].end.timed_out);
  CHECK(harness.reports[0].episode_alive);
  CHECK(harness.observed.destroyed == std::vector<std::size_t>{0});
  const auto result = batch.step(start + 1min, false);
  REQUIRE(result);
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{0, 1});
  CHECK(result->started == 2);
}

TEST_CASE("a stop request cancels every live episode and starts no more",
          "[batch][stop]") {
  Harness harness;
  // Both live episodes take a few pumps to honour the stop.
  harness.scripts[0] = {.finish_pumps_after_stop = 2};
  harness.scripts[1] = {.finish_pumps_after_stop = 3};
  auto batch = harness.batch(5, 2, 10s);

  CHECK_FALSE(batch.step(start, false));
  CHECK(batch.live() == 2);
  CHECK_FALSE(batch.step(start + 1ms, true));
  CHECK(harness.observed.stops == std::map<std::size_t, int>{{0, 1}, {1, 1}});
  // The request latches: a later false does not start queued jobs.
  CHECK_FALSE(batch.step(start + 2ms, false));
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{0});
  const auto result = batch.step(start + 3ms, false);
  REQUIRE(result);
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{0, 1});
  for (const auto &report : harness.reports) {
    CHECK(report.end.outcome == DriveOutcome::interrupted);
    CHECK_FALSE(report.end.timed_out);
    CHECK(report.episode_alive);
  }
  // stop() once per episode, however long the cancellation took.
  CHECK(harness.observed.stops == std::map<std::size_t, int>{{0, 1}, {1, 1}});
  CHECK(harness.factory_calls == std::vector<std::size_t>{0, 1});
  CHECK(harness.observed.alive == 0);
  CHECK(result->jobs == 5);
  CHECK(result->started == 2);
  CHECK(result->not_started == 3);
  CHECK(result->stopped);
  CHECK_FALSE(result->error);
}

TEST_CASE("a stop request before the first step starts nothing",
          "[batch][stop]") {
  Harness harness;
  auto batch = harness.batch(3, 2);
  const auto result = batch.step(start, true);
  REQUIRE(result);
  CHECK(harness.factory_calls.empty());
  CHECK(result->not_started == 3);
  CHECK(result->stopped);
}

TEST_CASE("a stop wins over a timeout already under way", "[batch][stop]") {
  Harness harness;
  harness.scripts[0] = {.finish_pumps_after_stop = 2};
  auto batch = harness.batch(1, 1, 10s);
  CHECK_FALSE(batch.step(start, false));
  CHECK_FALSE(batch.step(start + 10s, false));
  CHECK_FALSE(batch.step(start + 11s, true));
  const auto result = batch.step(start + 12s, true);
  REQUIRE(result);
  REQUIRE(harness.reports.size() == 1);
  CHECK(harness.reports[0].end.outcome == DriveOutcome::interrupted);
  // The deadline had passed, and the report still says so.
  CHECK(harness.reports[0].end.timed_out);
}

TEST_CASE("a factory failure aborts the batch and still reports live "
          "episodes",
          "[batch][abort]") {
  Harness harness;
  harness.scripts[0] = {.finish_on_pump = 3};
  harness.scripts[1] = {.finish_pumps_after_stop = 2};
  harness.fail = {2};
  auto batch = harness.batch(4, 2, 10s);

  CHECK_FALSE(batch.step(start, false)); // 0 and 1 start.
  CHECK_FALSE(batch.step(start + 1ms, false));
  // Job 0 ends on this pass; job 2 fails to start on the next one, which
  // stops job 1 in the same pass.
  CHECK_FALSE(batch.step(start + 2ms, false));
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{0});
  CHECK_FALSE(batch.step(start + 3ms, false));
  CHECK(harness.factory_calls == std::vector<std::size_t>{0, 1, 2});
  CHECK(harness.observed.stops == std::map<std::size_t, int>{{1, 1}});
  const auto result = batch.step(start + 4ms, false);
  REQUIRE(result);
  CHECK(harness.reported_jobs() == std::vector<std::size_t>{0, 1});
  CHECK(harness.reports[0].end.outcome == DriveOutcome::finished);
  CHECK(harness.reports[1].end.outcome == DriveOutcome::interrupted);
  // Job 3 is never asked for.
  CHECK(harness.factory_calls == std::vector<std::size_t>{0, 1, 2});
  CHECK(result->error == "job 2 failed");
  CHECK(result->started == 2);
  CHECK(result->not_started == 2);
  CHECK_FALSE(result->stopped);
}

TEST_CASE("a failure on the first job aborts with nothing reported",
          "[batch][abort]") {
  Harness harness;
  harness.fail = {0};
  auto batch = harness.batch(3, 2);
  const auto result = batch.step(start, false);
  REQUIRE(result);
  CHECK(harness.factory_calls == std::vector<std::size_t>{0});
  CHECK(harness.reports.empty());
  CHECK(result->error == "job 0 failed");
  CHECK(result->not_started == 3);
}

TEST_CASE("a factory that returns no episode aborts the batch",
          "[batch][abort]") {
  EpisodeBatch batch{2, 1, 10s,
                     [](std::size_t) -> std::expected<BatchEntry, std::string> {
                       return BatchEntry{};
                     }};
  const auto result = batch.step(start, false);
  REQUIRE(result);
  CHECK(result->error == "the episode factory returned no episode");
  CHECK(result->started == 0);
}

TEST_CASE("run sleeps only after a pass in which nothing progressed",
          "[batch][run]") {
  const auto run = [](const PumpStats stats, const int finish_on_pump) {
    Harness harness;
    harness.scripts[0] = {.finish_on_pump = finish_on_pump, .stats = stats};
    auto batch = harness.batch(1, 1);
    auto now = start;
    std::vector<EpisodeBatch::Clock::duration> sleeps;
    int polls = 0;
    const auto result = batch.run({
        .now = [&] { return now; },
        .sleep =
            [&](const EpisodeBatch::Clock::duration pause) {
              sleeps.push_back(pause);
              now += pause;
            },
        .stop_requested =
            [&] {
              ++polls;
              return false;
            },
    });
    CHECK(result.started == 1);
    CHECK(harness.reports.size() == 1);
    CHECK(polls == finish_on_pump);
    return sleeps;
  };

  // Pass 1 starts the job (progress); passes 2..4 pump an idle episode;
  // pass 5 finishes it and ends the batch without sleeping again.
  const auto idle = run({}, 5);
  CHECK(idle == std::vector<EpisodeBatch::Clock::duration>(
                    3, pigpen::agent::idle_pause));
  // An episode that keeps delivering callbacks is never slept on.
  CHECK(run({.callbacks_delivered = 1}, 5).empty());
  CHECK(run({.events_remaining = 2}, 5).empty());
}

TEST_CASE("run passes a stop request through and ends", "[batch][run]") {
  Harness harness;
  harness.scripts[0] = {.finish_pumps_after_stop = 1};
  auto batch = harness.batch(3, 1);
  auto now = start;
  int polls = 0;
  const auto result = batch.run({
      .now = [&] { return now; },
      .sleep = [&](const EpisodeBatch::Clock::duration pause) { now += pause; },
      .stop_requested = [&] { return ++polls >= 3; },
  });
  CHECK(result.stopped);
  CHECK(result.started == 1);
  CHECK(result.not_started == 2);
  REQUIRE(harness.reports.size() == 1);
  CHECK(harness.reports[0].end.outcome == DriveOutcome::interrupted);
}

TEST_CASE("a factory that throws aborts the batch like a factory failure",
          "[batch][abort]") {
  Harness harness;
  harness.scripts[0] = {.finish_pumps_after_stop = 2};
  int calls = 0;
  // Job 0 comes from the fake; job 1 throws.
  EpisodeBatch batch{
      3, 2, 10s,
      [&](const std::size_t job) -> std::expected<BatchEntry, std::string> {
        ++calls;
        if (job == 1) {
          throw std::runtime_error{"out of sessions"};
        }
        auto episode = std::make_shared<FakeEpisode>(job, harness.scripts[job],
                                                     harness.observed);
        return BatchEntry{
            .episode = std::move(episode),
            .on_end =
                [&harness](const EpisodeEnd &end) {
                  harness.reports.push_back({.end = end});
                },
        };
      }};
  CHECK_FALSE(batch.step(start, false));
  CHECK(calls == 2);
  const auto result = batch.step(start + 1ms, false);
  REQUIRE(result);
  CHECK(result->error == "could not start job 1: out of sessions");
  CHECK(result->started == 1);
  CHECK(result->not_started == 2);
  REQUIRE(harness.reports.size() == 1);
  CHECK(harness.reports[0].end.outcome == DriveOutcome::interrupted);
  CHECK(harness.observed.alive == 0);
  CHECK(calls == 2);
}

TEST_CASE("a report that throws is not retried and aborts the batch",
          "[batch][abort]") {
  Observed observed;
  std::vector<std::size_t> reports;
  EpisodeBatch batch{
      4, 2, 10s,
      [&](const std::size_t job) -> std::expected<BatchEntry, std::string> {
        auto episode = std::make_shared<FakeEpisode>(
            job,
            Script{.finish_on_pump =
                       job == 0 ? std::optional<int>{1} : std::nullopt,
                   .finish_pumps_after_stop = 2},
            observed);
        return BatchEntry{
            .episode = std::move(episode),
            .on_end =
                [&reports](const EpisodeEnd &end) {
                  reports.push_back(end.job);
                  if (end.job == 0) {
                    throw std::bad_alloc{};
                  }
                },
        };
      }};
  // Job 0 ends and its report throws; job 1, stepped later in the same
  // pass, is asked to stop at once.
  CHECK_FALSE(batch.step(start, false));
  CHECK(reports == std::vector<std::size_t>{0});
  CHECK(observed.destroyed == std::vector<std::size_t>{0});
  CHECK(observed.stops == std::map<std::size_t, int>{{1, 1}});
  const auto result = batch.step(start + 1ms, false);
  REQUIRE(result);
  CHECK(result->error == "could not report job 0: std::bad_alloc");
  CHECK(reports == std::vector<std::size_t>{0, 1});
  CHECK(result->started == 2);
  CHECK(result->not_started == 2);
  CHECK(observed.alive == 0);
  // The ended batch never reports again.
  CHECK(batch.step(start + 2ms, false));
  CHECK(reports.size() == 2);
}
