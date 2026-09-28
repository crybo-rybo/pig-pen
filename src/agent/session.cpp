/// @file session.cpp
/// @brief Session implementation: config validation, composition, and
/// reflection-based tool registration; the contract is in the header.
#include "agent/session.hpp"

#include "agent/episode_summary.hpp"
#include "agent/metrics_writer.hpp"
#include "agent/prompt.hpp"
#include "agent/scry_transport.hpp"
#include "agent/world_tool_binding.hpp"

#include <scry/scry.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pigpen::agent {

namespace {

/// @brief ASCII case-insensitive equality, as HTTP header names compare.
[[nodiscard]] bool header_name_equal(const std::string_view left,
                                     const std::string_view right) {
  return std::ranges::equal(left, right, [](const char a, const char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

} // namespace

struct Session::Impl {
  Impl(Config initial_config, std::string initial_rollout_id,
       RewardWeights initial_reward_weights,
       std::unique_ptr<MetricsWriter> initial_metrics,
       std::unique_ptr<world::World> initial_world,
       std::unique_ptr<WorldToolBinding> initial_tools,
       scry::Harness initial_harness, scry::Conversation initial_conversation)
      : config(std::move(initial_config)),
        rollout_id(std::move(initial_rollout_id)),
        reward_weights(initial_reward_weights), world(std::move(initial_world)),
        metrics(std::move(initial_metrics)),
        metrics_path(metrics ? std::optional{metrics->path()} : std::nullopt),
        tools(std::move(initial_tools)), harness(std::move(initial_harness)),
        conversation(std::move(initial_conversation)),
        transport(
            harness, conversation,
            {.on_tool_request =
                 [this](const scry::ToolRequest &) {
                   return tools->admit(!metrics_error.empty());
                 },
             .on_tool_call =
                 [this](const scry::ToolCall &call) { tools->observe(call); },
             .on_turn_finished =
                 [this] { turn_host_refused_calls = tools->complete_turn(); }}),
        runner(
            transport, config.turn_budget,
            [this] { return world->all_positive_items_eaten(); },
            {
                .on_turn_finished =
                    [this](const TurnRecord &record) {
                      // Staged by this turn's terminal delivery, which always
                      // precedes the runner applying its outcome.
                      const auto host_refused =
                          std::exchange(turn_host_refused_calls, 0U);
                      turns.push_back({
                          .record = record,
                          .calls = tally_turn_calls(record, host_refused),
                      });
                      if (!metrics) {
                        return;
                      }
                      if (auto status = metrics->record_turn(turns.back());
                          !status) {
                        metrics_error = std::move(status.error());
                      }
                      if (!metrics_error.empty()) {
                        static_cast<void>(runner.fail(metrics_error));
                      }
                    },
                .on_episode_finished =
                    [this](const EpisodeResult &) {
                      finished = std::chrono::steady_clock::now();
                      if (!metrics) {
                        return;
                      }
                      // The runner's snapshot already carries the result.
                      auto summary = summarize_episode(
                          *world, activities, turns, runner.snapshot(),
                          elapsed(), reward_weights);
                      summary.rollout_id = rollout_id;
                      if (auto status = metrics->finish(summary); !status) {
                        metrics_error = std::move(status.error());
                      }
                    },
            },
            [this] { return activities.size(); }) {
    tools->on_activity = [this](ToolActivity activity) {
      activity.tick = activities.size() + 1U;
      activity.turn = runner.snapshot().turns_used + 1U;
      activities.push_back(std::move(activity));
      if (!metrics) {
        return;
      }
      if (auto recorded = metrics->record_tool(activities.back()); !recorded) {
        metrics_error = std::move(recorded.error());
      }
    };
  }

  [[nodiscard]] std::chrono::milliseconds elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        finished.value_or(std::chrono::steady_clock::now()) - created);
  }

  Config config;
  std::string rollout_id;
  RewardWeights reward_weights;
  std::chrono::steady_clock::time_point created{
      std::chrono::steady_clock::now()};
  std::optional<std::chrono::steady_clock::time_point> finished{};
  std::unique_ptr<world::World> world;
  ToolActivityFeed activities{};
  std::vector<EpisodeTurn> turns{};
  // Admission refusals of the turn whose terminal delivery just arrived.
  std::uint32_t turn_host_refused_calls{};
  // Null when the session writes no log.
  std::unique_ptr<MetricsWriter> metrics;
  std::optional<std::filesystem::path> metrics_path;
  // Destruction runs in reverse: bindings and world outlive the harness.
  std::unique_ptr<WorldToolBinding> tools;
  scry::Harness harness;
  scry::Conversation conversation;
  ScryTurnTransport transport;
  EpisodeRunner runner;
  std::string metrics_error{};
};

std::expected<std::shared_ptr<Session>, std::string>
Session::create(Config config, SessionOptions options) {
  if (config.turn_budget == 0) {
    return std::unexpected("turn budget must be greater than zero");
  }
  if (config.turn_budget > turn_budget_limit) {
    return std::unexpected("turn budget must not exceed " +
                           std::to_string(turn_budget_limit));
  }
  if (config.max_tool_rounds > tool_rounds_limit) {
    return std::unexpected("maximum tool rounds must not exceed " +
                           std::to_string(tool_rounds_limit));
  }
  for (const auto &[name, value] : options.request_headers) {
    if (header_name_equal(name, rollout_header_name)) {
      return std::unexpected("request header " + std::string{name} +
                             " is reserved for the rollout id");
    }
  }
  auto conversation = scry::Conversation::create(
      {.system_prompt = build_system_prompt(config)});
  if (!conversation) {
    return std::unexpected(conversation.error().message);
  }
  auto world = std::make_unique<world::World>(config.seed);
  auto tools = std::make_unique<WorldToolBinding>(*world, config);
  auto registry = tools->registry();
  if (!registry) {
    return std::unexpected(registry.error().message);
  }
  // Scry validates the request headers, including collisions with its own.
  auto harness =
      scry::Harness::create(scry_config(config, options), std::move(*registry));
  if (!harness) {
    return std::unexpected(harness.error().message);
  }
  // Opened last so a rejected config leaves no log behind.
  std::unique_ptr<MetricsWriter> metrics;
  if (options.log_directory) {
    auto created = MetricsWriter::create(*options.log_directory, config,
                                         std::move(options.prompt_variant),
                                         options.rollout_id);
    if (!created) {
      return std::unexpected(std::move(created.error()));
    }
    metrics = std::move(*created);
  }

  return std::shared_ptr<Session>{new Session{std::make_unique<Impl>(
      std::move(config), std::move(options.rollout_id), options.reward_weights,
      std::move(metrics), std::move(world), std::move(tools),
      std::move(*harness), std::move(*conversation))}};
}

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Session::~Session() {
  // The activity sink uses the runner and metrics writer. Flush while both
  // are alive; member teardown cancels and disconnects the transport next.
  impl_->tools->flush_pending_activity();
}

PumpStats Session::pump() {
  const auto stats = impl_->harness.update({
      .time_budget = std::chrono::milliseconds{2},
      .max_callbacks = 32,
  });
  // Turn completion is the single place that ends an episode on logging
  // failure, after admission has refused further actions and results commit.
  impl_->runner.tick();
  return {
      .callbacks_delivered = stats.callbacks_delivered,
      .events_remaining = stats.events_remaining,
  };
}

bool Session::finished() const {
  return impl_->runner.snapshot().state == RunState::finished;
}

bool Session::play() { return impl_->runner.play(); }
bool Session::pause() { return impl_->runner.pause(); }
bool Session::stop() { return impl_->runner.stop(); }

std::uint64_t Session::queue_user_input(std::string message) {
  return impl_->runner.queue_user_input(std::move(message));
}

bool Session::remove_pending_user_input(const std::uint64_t id) {
  return impl_->runner.remove_pending_user_input(id);
}

void Session::clear_pending_user_inputs() {
  impl_->runner.clear_pending_user_inputs();
}

const Config &Session::config() const noexcept { return impl_->config; }
const world::World &Session::world() const noexcept { return *impl_->world; }
const ToolActivityFeed &Session::tool_activities() const noexcept {
  return impl_->activities;
}
const EpisodeRunner &Session::runner() const noexcept { return impl_->runner; }
const std::vector<EpisodeTurn> &Session::turns() const noexcept {
  return impl_->turns;
}

std::chrono::milliseconds Session::elapsed() const { return impl_->elapsed(); }

const std::string &Session::rollout_id() const noexcept {
  return impl_->rollout_id;
}

const RewardWeights &Session::reward_weights() const noexcept {
  return impl_->reward_weights;
}

const std::optional<std::filesystem::path> &
Session::metrics_path() const noexcept {
  return impl_->metrics_path;
}

const std::string &Session::metrics_error() const noexcept {
  return impl_->metrics_error;
}

} // namespace pigpen::agent
