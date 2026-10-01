/// @file session.cpp
/// @brief Session implementation: config validation, composition, and
/// reflection-based tool registration; the contract is in the header.
#include "agent/session.hpp"

#include "agent/metrics_writer.hpp"
#include "agent/scry_transport.hpp"
#include "agent/world_tool_binding.hpp"
#include "core/prompt.hpp"

#include <scry/scry.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace pigpen::agent {

struct Session::Impl {
  Impl(core::Config initial_config,
       std::unique_ptr<MetricsWriter> initial_metrics,
       std::unique_ptr<world::World> initial_world,
       std::unique_ptr<WorldToolBinding> initial_tools,
       scry::Harness initial_harness, scry::Conversation initial_conversation)
      : config(std::move(initial_config)), world(std::move(initial_world)),
        metrics(std::move(initial_metrics)), tools(std::move(initial_tools)),
        harness(std::move(initial_harness)),
        conversation(std::move(initial_conversation)),
        transport(
            harness, conversation,
            {.on_tool_request =
                 [this](const scry::ToolRequest &) {
                   return tools->admit(!metrics_error.empty());
                 },
             .on_tool_call =
                 [this](const scry::ToolCall &call) { tools->observe(call); },
             .on_turn_finished = [this] { tools->flush_pending_activity(); }}),
        runner(
            transport, config.turn_budget,
            [this] { return world->all_positive_items_eaten(); },
            {
                .on_turn_finished =
                    [this](const core::TurnRecord &record) {
                      if (auto status = metrics->record_turn(record); !status) {
                        metrics_error = std::move(status.error());
                      }
                      if (!metrics_error.empty()) {
                        static_cast<void>(runner.fail(metrics_error));
                      }
                    },
                .on_episode_finished =
                    [this](const core::EpisodeResult &result) {
                      if (auto status = metrics->finish(result, world->score());
                          !status) {
                        metrics_error = std::move(status.error());
                      }
                    },
            },
            [this] { return activities.size(); }) {
    tools->on_activity = [this](core::ToolActivity activity) {
      activity.tick = activities.size() + 1U;
      activity.turn = runner.snapshot().turns_used + 1U;
      activities.push_back(std::move(activity));
      if (auto recorded = metrics->record_tool(activities.back()); !recorded) {
        metrics_error = std::move(recorded.error());
      }
    };
  }

  core::Config config;
  std::unique_ptr<world::World> world;
  core::ToolActivityFeed activities{};
  std::unique_ptr<MetricsWriter> metrics;
  // Destruction runs in reverse: bindings and world outlive the harness.
  std::unique_ptr<WorldToolBinding> tools;
  scry::Harness harness;
  scry::Conversation conversation;
  ScryTurnTransport transport;
  core::EpisodeRunner runner;
  std::string metrics_error{};
};

std::expected<std::shared_ptr<Session>, std::string>
Session::create(core::Config config, std::filesystem::path log_directory,
                std::string prompt_variant) {
  if (config.turn_budget == 0) {
    return std::unexpected("turn budget must be greater than zero");
  }
  if (config.turn_budget > core::turn_budget_limit) {
    return std::unexpected("turn budget must not exceed " +
                           std::to_string(core::turn_budget_limit));
  }
  if (config.max_tool_rounds > core::tool_rounds_limit) {
    return std::unexpected("maximum tool rounds must not exceed " +
                           std::to_string(core::tool_rounds_limit));
  }
  auto conversation = scry::Conversation::create(
      {.system_prompt = core::build_system_prompt(config)});
  if (!conversation) {
    return std::unexpected(conversation.error().message);
  }
  auto world = std::make_unique<world::World>(config.seed);
  auto tools = std::make_unique<WorldToolBinding>(*world, config);
  auto registry = tools->registry();
  if (!registry) {
    return std::unexpected(registry.error().message);
  }
  const auto *api_key = std::getenv("PIGPEN_API_KEY");
  auto harness = scry::Harness::create(
      scry_config(config, api_key == nullptr ? "" : api_key),
      std::move(*registry));
  if (!harness) {
    return std::unexpected(harness.error().message);
  }
  // Opened last so a rejected config leaves no log behind.
  auto metrics =
      MetricsWriter::create(log_directory, config, std::move(prompt_variant));
  if (!metrics) {
    return std::unexpected(std::move(metrics.error()));
  }

  return std::shared_ptr<Session>{new Session{std::make_unique<Impl>(
      std::move(config), std::move(*metrics), std::move(world),
      std::move(tools), std::move(*harness), std::move(*conversation))}};
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

const core::Config &Session::config() const noexcept { return impl_->config; }
const world::World &Session::world() const noexcept { return *impl_->world; }
const core::ToolActivityFeed &Session::tool_activities() const noexcept {
  return impl_->activities;
}
const core::EpisodeRunner &Session::runner() const noexcept {
  return impl_->runner;
}

const std::filesystem::path &Session::metrics_path() const noexcept {
  return impl_->metrics->path();
}

const std::string &Session::metrics_error() const noexcept {
  return impl_->metrics_error;
}

} // namespace pigpen::agent
