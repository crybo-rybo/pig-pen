/// @file session.cpp
/// @brief Session implementation: config validation, the llamad connection,
/// and composition; the contract is in the header.
#include "agent/session.hpp"

#include "agent/metrics_writer.hpp"
#include "agent/world_tool_binding.hpp"
#include "core/prompt.hpp"
#include "core/tool_loop.hpp"
#include "llamad/llamad_client.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief The bounds that Session::create enforces before it connects.
[[nodiscard]] std::expected<void, std::string>
validate(const core::Config &config) {
  if (config.socket_path.empty()) {
    return std::unexpected("the llamad socket path must not be empty");
  }
  if (config.turn_budget == 0) {
    return std::unexpected("turn budget must be greater than zero");
  }
  if (config.turn_budget > core::turn_budget_limit) {
    return std::unexpected("turn budget must not exceed " +
                           std::to_string(core::turn_budget_limit));
  }
  if (config.max_tool_rounds == 0) {
    return std::unexpected("maximum tool rounds must be greater than zero");
  }
  if (config.max_tool_rounds > core::tool_rounds_limit) {
    return std::unexpected("maximum tool rounds must not exceed " +
                           std::to_string(core::tool_rounds_limit));
  }
  if (config.max_output_tokens == 0 ||
      config.max_output_tokens > core::max_output_tokens_limit) {
    return std::unexpected("maximum output tokens must be in the range 1.." +
                           std::to_string(core::max_output_tokens_limit));
  }
  if (!std::isfinite(config.temperature) || config.temperature < 0.0 ||
      config.temperature > 2.0) {
    return std::unexpected(
        "temperature must be a finite number in the range 0.0..2.0");
  }
  return {};
}

} // namespace

struct Session::Impl {
  Impl(core::Config initial_config,
       std::unique_ptr<llamad::LlamadChatClient> initial_client,
       std::unique_ptr<world::World> initial_world,
       std::shared_ptr<WorldToolBinding> initial_binding,
       std::unique_ptr<MetricsWriter> initial_metrics)
      : config(std::move(initial_config)), world(std::move(initial_world)),
        metrics(std::move(initial_metrics)),
        binding(std::move(initial_binding)),
        dispatcher(binding, [this] { return !metrics_error.empty(); }),
        client(std::move(initial_client)),
        tool_loop(
            *client, dispatcher, core::build_system_prompt(config),
            {.temperature = config.temperature,
             .max_tokens = config.max_output_tokens,
             .seed = config.sampling_seed},
            {.max_tool_rounds = config.max_tool_rounds,
             .max_tool_calls_per_turn = core::max_world_tool_calls_per_turn}),
        runner(
            tool_loop, config.turn_budget,
            [this] { return world->all_positive_items_eaten(); },
            {
                .on_turn_finished =
                    [this](const core::TurnRecord &record) {
                      totals.input += record.input_tokens;
                      totals.output += record.output_tokens;
                      totals.cached_input += record.cached_input_tokens;
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
    dispatcher.on_activity = [this](core::ToolActivity activity) {
      activity.tick = activities.size() + 1U;
      activity.turn = runner.snapshot().turns_used + 1U;
      activities.push_back(std::move(activity));
      if (auto recorded = metrics->record_tool(activities.back()); !recorded) {
        metrics_error = std::move(recorded.error());
      }
    };
  }

  // Destruction runs in reverse: the runner and the tool loop go first and
  // cancel the active request, then the client joins its worker and drops
  // undelivered events, and only then do the tools and the world go.
  core::Config config;
  std::unique_ptr<world::World> world;
  core::ToolActivityFeed activities{};
  std::unique_ptr<MetricsWriter> metrics;
  std::string metrics_error{};
  TokenTotals totals{};
  std::shared_ptr<WorldToolBinding> binding;
  WorldToolDispatcher dispatcher;
  std::unique_ptr<llamad::LlamadChatClient> client;
  core::ToolLoopTransport tool_loop;
  core::EpisodeRunner runner;
};

std::expected<std::shared_ptr<Session>, std::string>
Session::create(core::Config config, std::filesystem::path log_directory,
                std::string prompt_variant) {
  if (auto valid = validate(config); !valid) {
    return std::unexpected(std::move(valid.error()));
  }
  auto client = llamad::LlamadChatClient::connect(config.socket_path);
  if (!client) {
    return std::unexpected(std::move(client.error()));
  }
  auto world = std::make_unique<world::World>(config.seed);
  auto binding = std::make_shared<WorldToolBinding>(*world, config);
  // Opened last so a rejected config or daemon leaves no log behind.
  auto metrics =
      MetricsWriter::create(log_directory, config, (*client)->model_info(),
                            std::move(prompt_variant));
  if (!metrics) {
    return std::unexpected(std::move(metrics.error()));
  }

  return std::shared_ptr<Session>{new Session{std::make_unique<Impl>(
      std::move(config), std::move(*client), std::move(world),
      std::move(binding), std::move(*metrics))}};
}

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Session::~Session() = default;

PumpStats Session::pump() {
  const auto stats = impl_->client->pump(32);
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
const core::ModelInfo &Session::model_info() const noexcept {
  return impl_->client->model_info();
}
const world::World &Session::world() const noexcept { return *impl_->world; }
const core::ToolActivityFeed &Session::tool_activities() const noexcept {
  return impl_->activities;
}
const core::EpisodeRunner &Session::runner() const noexcept {
  return impl_->runner;
}
const TokenTotals &Session::token_totals() const noexcept {
  return impl_->totals;
}

const std::filesystem::path &Session::metrics_path() const noexcept {
  return impl_->metrics->path();
}

const std::string &Session::metrics_error() const noexcept {
  return impl_->metrics_error;
}

} // namespace pigpen::agent
