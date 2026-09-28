/// @file session_bench.cpp
/// @brief Measures what the worker's scheduling costs: creating and
/// destroying a Session, and the latency of one EpisodeBatch pass over P
/// live sessions. Built only with PIGPEN_BUILD_BENCH; driven against a
/// loopback stub by tests/bench/run_session_bench.py, which documents how
/// to run it. Every result is one JSON line on stdout.
///
///   pigpen_session_bench create URL COUNT
///   pigpen_session_bench batch URL PARALLEL EPISODES TURNS
#include "agent/episode_batch.hpp"
#include "agent/session.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using pigpen::agent::BatchEntry;
using pigpen::agent::EpisodeBatch;
using pigpen::agent::Session;

/// @brief Microsecond samples, summarised as a JSON object.
class Samples final {
public:
  void add(const Clock::duration duration) {
    values_.push_back(
        std::chrono::duration<double, std::micro>(duration).count());
  }

  [[nodiscard]] std::string json() {
    if (values_.empty()) {
      return R"({"n":0})";
    }
    std::ranges::sort(values_);
    double sum = 0;
    for (const auto value : values_) {
      sum += value;
    }
    const auto at = [this](const double quantile) {
      const auto index = static_cast<std::size_t>(
          quantile * static_cast<double>(values_.size()));
      return values_[std::min(values_.size() - 1, index)];
    };
    return std::format(
        R"({{"n":{},"mean_us":{:.1f},"p50_us":{:.1f},"p99_us":{:.1f},"max_us":{:.1f}}})",
        values_.size(), sum / static_cast<double>(values_.size()), at(0.5),
        at(0.99), values_.back());
  }

private:
  std::vector<double> values_{};
};

[[nodiscard]] pigpen::agent::Config config(const std::string &url,
                                           const std::uint32_t turns,
                                           const std::uint64_t seed) {
  pigpen::agent::Config result;
  result.base_url = url;
  result.model = "bench";
  result.turn_budget = turns;
  result.max_tool_rounds = 2;
  result.seed = seed;
  return result;
}

[[nodiscard]] std::shared_ptr<Session> create(const std::string &url,
                                              const std::uint32_t turns,
                                              const std::uint64_t seed) {
  auto session =
      Session::create(config(url, turns, seed),
                      {.rollout_id = "bench/" + std::to_string(seed) + "/0"});
  if (!session) {
    throw std::runtime_error{session.error()};
  }
  return std::move(*session);
}

/// @brief Create and destroy sessions that never send a request, then play
/// one-turn episodes one at a time, timing creation and destruction apart
/// from the episode.
int bench_create(const std::string &url, const std::size_t count) {
  Samples idle_create;
  Samples idle_destroy;
  for (std::size_t index = 0; index < count; ++index) {
    auto started = Clock::now();
    auto session = create(url, 1, index);
    idle_create.add(Clock::now() - started);
    started = Clock::now();
    session.reset();
    idle_destroy.add(Clock::now() - started);
  }

  Samples created;
  Samples played;
  Samples destroyed;
  for (std::size_t index = 0; index < count; ++index) {
    auto started = Clock::now();
    auto session = create(url, 1, index);
    created.add(Clock::now() - started);
    started = Clock::now();
    static_cast<void>(session->play());
    while (!session->finished()) {
      if (session->pump().callbacks_delivered == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds{50});
      }
    }
    played.add(Clock::now() - started);
    started = Clock::now();
    session.reset();
    destroyed.add(Clock::now() - started);
  }
  std::cout
      << std::format(
             R"({{"bench":"create","count":{},"idle_create":{},"idle_destroy":{},)"
             R"("create":{},"one_turn_episode":{},"destroy_after_episode":{}}})",
             count, idle_create.json(), idle_destroy.json(), created.json(),
             played.json(), destroyed.json())
      << '\n';
  return 0;
}

/// @brief A Session whose pump() calls are timed.
class TimedEpisode final : public pigpen::agent::IDrivableEpisode {
public:
  TimedEpisode(std::shared_ptr<Session> session, Samples &busy,
               Clock::duration &pumping)
      : session_(std::move(session)), busy_(busy), pumping_(pumping) {}

  [[nodiscard]] pigpen::agent::PumpStats pump() override {
    const auto started = Clock::now();
    const auto stats = session_->pump();
    const auto spent = Clock::now() - started;
    pumping_ += spent;
    if (stats.callbacks_delivered > 0) {
      busy_.add(spent);
    }
    return stats;
  }
  [[nodiscard]] bool finished() const override { return session_->finished(); }
  bool stop() override { return session_->stop(); }

private:
  std::shared_ptr<Session> session_;
  Samples &busy_;
  Clock::duration &pumping_;
};

/// @brief Run EPISODES episodes PARALLEL at a time through EpisodeBatch, as
/// the worker does, timing every pass that did not end in the idle sleep.
int bench_batch(const std::string &url, const std::size_t parallel,
                const std::size_t episodes, const std::uint32_t turns) {
  Samples passes;
  Samples busy_pumps;
  Samples created;
  Clock::duration pumping{};
  EpisodeBatch batch{
      episodes, parallel, std::chrono::seconds{300},
      [&](const std::size_t job) -> std::expected<BatchEntry, std::string> {
        const auto started = Clock::now();
        auto session = create(url, turns, job);
        created.add(Clock::now() - started);
        if (!session->play()) {
          return std::unexpected("could not play");
        }
        return BatchEntry{
            .episode = std::make_shared<TimedEpisode>(std::move(session),
                                                      busy_pumps, pumping),
            .on_end = [](const pigpen::agent::EpisodeEnd &) {},
        };
      }};
  std::optional<Clock::time_point> last_pass;
  bool slept = false;
  std::size_t sleeps = 0;
  const auto started = Clock::now();
  const auto result = batch.run({
      .now =
          [&] {
            const auto now = Clock::now();
            if (last_pass && !slept) {
              passes.add(now - *last_pass);
            }
            last_pass = now;
            slept = false;
            return now;
          },
      .sleep =
          [&](const Clock::duration pause) {
            ++sleeps;
            std::this_thread::sleep_for(pause);
            slept = true;
          },
      .stop_requested = [] { return false; },
  });
  const auto wall = Clock::now() - started;
  const auto wall_s = std::chrono::duration<double>(wall).count();
  std::cout
      << std::format(
             R"({{"bench":"batch","parallel":{},"episodes":{},"turns":{},)"
             R"("started":{},"error":"{}","wall_ms":{:.0f},"episodes_per_s":{:.1f},)"
             R"("pump_busy_percent":{:.1f},"idle_sleeps":{},"pass":{},)"
             R"("pump_with_callbacks":{},"create":{}}})",
             parallel, episodes, turns, result.started,
             result.error.value_or(""), wall_s * 1000,
             static_cast<double>(episodes) / wall_s,
             100 * std::chrono::duration<double>(pumping).count() / wall_s,
             sleeps, passes.json(), busy_pumps.json(), created.json())
      << '\n';
  return result.error ? 1 : 0;
}

[[nodiscard]] std::size_t number(const std::string_view text) {
  std::size_t value{};
  const auto *const end = text.data() + text.size();
  if (const auto [last, error] = std::from_chars(text.data(), end, value);
      error != std::errc{} || last != end) {
    throw std::runtime_error{"not a number: " + std::string{text}};
  }
  return value;
}

} // namespace

int main(const int argc, char **argv) {
  const std::vector<std::string_view> arguments(argv, argv + argc);
  try {
    if (arguments.size() == 4 && arguments[1] == "create") {
      return bench_create(std::string{arguments[2]}, number(arguments[3]));
    }
    if (arguments.size() == 6 && arguments[1] == "batch") {
      return bench_batch(std::string{arguments[2]}, number(arguments[3]),
                         number(arguments[4]),
                         static_cast<std::uint32_t>(number(arguments[5])));
    }
  } catch (const std::exception &error) {
    std::cerr << "bench error: " << error.what() << '\n';
    return 1;
  }
  std::cerr << "usage: pigpen_session_bench create URL COUNT\n"
               "       pigpen_session_bench batch URL PARALLEL EPISODES "
               "TURNS\n";
  return 2;
}
