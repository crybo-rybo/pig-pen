/// @file session.cpp
/// @brief Session implementation: config validation, composition, and
/// reflection-based tool registration; the contract is in the header.
#include "agent/session.hpp"

#include "agent/metrics_writer.hpp"
#include "agent/prompt.hpp"
#include "agent/scry_transport.hpp"
#include "agent/world_tool_binding.hpp"

#include <scry/scry.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief Read an environment variable; missing means empty.
[[nodiscard]] std::string environment(const char *name) {
  const auto *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

} // namespace

class Session::Impl final {
public:
  Impl(Config initial_config, std::unique_ptr<MetricsWriter> initial_metrics,
       std::unique_ptr<world::World> initial_world,
       std::unique_ptr<WorldToolBinding> initial_tools,
       scry::Harness initial_harness, scry::Conversation initial_conversation)
      : config(std::move(initial_config)), world(std::move(initial_world)),
        metrics(std::move(initial_metrics)), tools(std::move(initial_tools)),
        harness(std::move(initial_harness)),
        conversation(std::move(initial_conversation)),
        transport(
            this->harness, this->conversation,
            {.on_tool_request = [this](const scry::ToolRequest &)
                 -> std::optional<scry::ToolRejection> {
               if (!metrics_error.empty()) {
                 return scry::ToolRejection{
                     .model_message =
                         "World tools are unavailable because the episode log "
                         "failed. Summarize the actions already taken."};
               }
               if (world->all_positive_items_eaten()) {
                 return scry::ToolRejection{
                     .model_message =
                         "All positive-value items have been eaten. Summarize "
                         "the completed episode without more tools."};
               }
               return std::nullopt;
             },
             .on_tool_call =
                 [this](const scry::ToolCall &call) { tools->observe(call); },
             .on_turn_finished = [this] { tools->finish_turn(); }}),
        runner(
            transport, static_cast<std::uint32_t>(this->config.turn_budget),
            [this] { return world->all_positive_items_eaten(); },
            {
                .on_turn_finished =
                    [this](const TurnRecord &record) {
                      if (auto status = this->metrics->record_turn(record);
                          !status) {
                        metrics_error = std::move(status.error());
                      }
                      if (!metrics_error.empty()) {
                        static_cast<void>(runner.fail(metrics_error));
                      }
                    },
                .on_episode_finished =
                    [this](const EpisodeResult &result) {
                      if (auto status =
                              this->metrics->finish(result, world->score());
                          !status) {
                        metrics_error = std::move(status.error());
                      }
                    },
            },
            [this] { return activities.size(); }) {
    tools->on_activity = [this](ToolActivity activity) {
      activity.tick = activities.size() + 1U;
      activity.turn = runner.snapshot().turns_used + 1U;
      activities.push_back(std::move(activity));
      if (auto recorded = metrics->record_tool(activities.back()); !recorded) {
        metrics_error = std::move(recorded.error());
      }
    };
  }

  Config config;
  std::unique_ptr<world::World> world;
  ToolActivityFeed activities{};
  std::unique_ptr<MetricsWriter> metrics;
  // Destruction runs in reverse: bindings and world outlive the harness.
  std::unique_ptr<WorldToolBinding> tools;
  scry::Harness harness;
  scry::Conversation conversation;
  ScryTurnTransport transport;
  EpisodeRunner runner;
  std::string metrics_error{};
};

std::expected<std::shared_ptr<Session>, std::string>
Session::create(Config config, std::filesystem::path log_directory,
                std::string prompt_variant) {
  if (config.base_url.empty()) {
    return std::unexpected("base URL cannot be empty");
  }
  if (config.model.empty()) {
    return std::unexpected("model cannot be empty");
  }
  if (config.turn_budget == 0) {
    return std::unexpected("turn budget must be greater than zero");
  }
  if (config.turn_budget > 10'000) {
    return std::unexpected("turn budget must not exceed 10000");
  }
  if (config.max_tool_rounds == 0) {
    return std::unexpected("maximum tool rounds must be greater than zero");
  }
  if (config.max_tool_rounds > 64) {
    return std::unexpected("maximum tool rounds must not exceed 64");
  }
  if (config.max_output_tokens == 0) {
    return std::unexpected("maximum output tokens must be greater than zero");
  }
  if (!std::isfinite(config.temperature) || config.temperature < 0.0 ||
      config.temperature > 2.0) {
    return std::unexpected("temperature must be finite and in the range 0..2");
  }

  // Let scry reject a bad provider config before anything with a side effect
  // happens: validate() runs create()'s checks without starting a worker, and
  // the metrics log below is only opened once the whole config is known good.
  const auto provider = scry_config(config, environment("PIGPEN_API_KEY"));
  if (auto valid = scry::Harness::validate(provider); !valid) {
    return std::unexpected(valid.error().message);
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
  auto harness = scry::Harness::create(provider, std::move(*registry));
  if (!harness) {
    return std::unexpected(harness.error().message);
  }
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

Session::~Session() { impl_->tools->finish_turn(); }

PumpStats Session::pump() {
  const auto stats = impl_->harness.update({
      .time_budget = std::chrono::milliseconds{2},
      .max_callbacks = 32,
  });
  // Admission refuses subsequent actions as soon as logging fails. Keep
  // pumping until the turn commits its existing results before ending the
  // episode.
  if (!impl_->metrics_error.empty() &&
      !impl_->runner.snapshot().turn_in_flight) {
    static_cast<void>(impl_->runner.fail(impl_->metrics_error));
  } else {
    impl_->runner.tick();
  }
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

const Config &Session::config() const noexcept { return impl_->config; }
const world::World &Session::world() const noexcept { return *impl_->world; }
const ToolActivityFeed &Session::tool_activities() const noexcept {
  return impl_->activities;
}
const EpisodeRunner &Session::runner() const noexcept { return impl_->runner; }
EpisodeRunner &Session::runner() noexcept { return impl_->runner; }
std::size_t Session::tool_call_count() const noexcept {
  return impl_->activities.size();
}

const std::filesystem::path &Session::metrics_path() const noexcept {
  return impl_->metrics->path();
}

const std::string &Session::metrics_error() const noexcept {
  return impl_->metrics_error;
}

} // namespace pigpen::agent
