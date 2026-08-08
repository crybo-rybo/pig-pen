#include "agent/session.hpp"

#include "agent/metrics_writer.hpp"
#include "agent/prompt.hpp"
#include "agent/reflected_json.hpp"
#include "agent/scry_transport.hpp"
#include "agent/world_tools.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <scry/reflection.hpp>
#include <scry/scry.hpp>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pigpen::agent {
namespace {

[[nodiscard]] std::string environment(const char *name) {
  const auto *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] scry::Result<scry::Harness> create_harness(const Config &config) {
  return scry::Harness::create({
      .base_url = config.base_url,
      .api_key = environment("PIGPEN_API_KEY"),
      .model = config.model,
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling = {.temperature = config.temperature,
                   .top_p = std::nullopt,
                   .max_tokens = config.max_output_tokens},
      // Explicitly disabling hidden reasoning keeps bounded local runs finite
      // and leaves the visible transcript focused on actions across providers.
      .reasoning_mode = scry::ReasoningMode::disabled,
      .retry = {},
      .timeouts = {},
      .limits = {},
      .max_tool_rounds = config.max_tool_rounds,
      .tls_verify_peer = true,
  });
}

} // namespace

class Session::Impl final {
public:
  Impl(Config initial_config, std::unique_ptr<MetricsWriter> initial_metrics,
       scry::Harness initial_harness, scry::Conversation initial_conversation)
      : config(std::move(initial_config)),
        world(std::make_shared<world::World>(this->config.seed)),
        metrics(std::move(initial_metrics)),
        harness(std::move(initial_harness)),
        conversation(std::move(initial_conversation)),
        tools(*world, this->config),
        transport(this->harness, this->conversation),
        runner(
            transport, static_cast<std::uint32_t>(this->config.turn_budget),
            [this] { return world->all_positive_items_eaten(); },
            {
                .on_turn_finished =
                    [this](const TurnRecord &record) {
                      if (auto status = this->metrics->record_turn(record);
                          !status) {
                        metrics_error = std::move(status.error());
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
            [this] { return events.size(); }) {}

  Config config;
  std::shared_ptr<world::World> world;
  EventFeed events{};
  std::unique_ptr<MetricsWriter> metrics;
  scry::Harness harness;
  scry::Conversation conversation;
  WorldTools tools;
  ScryTurnTransport transport;
  EpisodeRunner runner;
  std::uint64_t next_tool_tick{1};
  std::string metrics_error{};
  bool metrics_failure_pending{};
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

  auto harness = create_harness(config);
  if (!harness) {
    return std::unexpected(harness.error().message);
  }
  auto conversation = scry::Conversation::create(
      {.system_prompt = build_system_prompt(config)});
  if (!conversation) {
    return std::unexpected(conversation.error().message);
  }
  auto metrics =
      MetricsWriter::create(log_directory, config, std::move(prompt_variant));
  if (!metrics) {
    return std::unexpected(std::move(metrics.error()));
  }

  auto session = std::shared_ptr<Session>{new Session{
      std::make_unique<Impl>(std::move(config), std::move(*metrics),
                             std::move(*harness), std::move(*conversation))}};
  if (auto registered = session->register_tools(); !registered) {
    return std::unexpected(std::move(registered.error()));
  }
  return session;
}

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Session::~Session() = default;

std::expected<void, std::string> Session::register_tools() {
  const std::weak_ptr<Session> weak_session{shared_from_this()};
  const auto add = [this, weak_session]<typename Arguments, typename Invoke>(
                       scry::reflection::ToolMetadata metadata, Invoke invoke) {
    using Execution = std::invoke_result_t<Invoke &, WorldTools &, Arguments>;
    using Response = typename Execution::response_type;
    auto name = metadata.name;
    return scry::reflection::add<Arguments>(
        impl_->harness.tools(), std::move(metadata),
        [weak_session, name = std::move(name), invoke = std::move(invoke)](
            Arguments arguments) mutable -> scry::Result<Response> {
          const auto session = weak_session.lock();
          if (!session) {
            return std::unexpected(scry::Error{
                .category = scry::ErrorCategory::invalid_state,
                .message = "pig-pen session no longer exists",
            });
          }
          const auto snapshot = session->impl_->runner.snapshot();
          const auto turn = snapshot.turns_used + 1U;
          auto arguments_json = reflected_json(arguments);
          session->impl_->tools.begin_turn(turn);
          auto execution =
              std::invoke(invoke, session->impl_->tools, std::move(arguments));
          auto response_json = reflected_json(execution.response);
          session->impl_->events.push_back(WorldEvent{
              .tick = session->impl_->next_tool_tick++,
              .turn = turn,
              .tool = name,
              .arguments = std::move(arguments_json),
              .result = std::move(response_json),
              .before = execution.before,
              .after = execution.after,
              .direction = execution.direction,
              .action_executed = execution.action_executed,
              .eaten = execution.eaten,
          });
          const auto &event = session->impl_->events.back();
          if (auto recorded = session->impl_->metrics->record_tool(
                  event, session->impl_->world->score());
              !recorded) {
            session->impl_->metrics_error = std::move(recorded.error());
            session->impl_->metrics_failure_pending = true;
          }
          return std::move(execution.response);
        });
  };

  if (auto status = add.template operator()<DirectionArguments>(
          {
              .name = "move",
              .description = "Move one cell north, south, east, or west.",
          },
          &WorldTools::move);
      !status) {
    return std::unexpected(status.error().message);
  }
  if (auto status = add.template operator()<DirectionArguments>(
          {
              .name = "look",
              .description = "Scan every cell in one direction to the wall.",
          },
          &WorldTools::look);
      !status) {
    return std::unexpected(status.error().message);
  }
  if (auto status = add.template operator()<EatArguments>(
          {
              .name = "eat",
              .description = "Eat the item on the current cell, if present.",
          },
          &WorldTools::eat);
      !status) {
    return std::unexpected(status.error().message);
  }
  return {};
}

PumpStats Session::pump() {
  const auto stats = impl_->harness.update({
      .time_budget = std::chrono::milliseconds{2},
      .max_callbacks = 32,
  });
  if (std::exchange(impl_->metrics_failure_pending, false)) {
    // The world action and its typed response have already committed. Fail the
    // episode only after Harness::update returns so the model receives that
    // truthful response and cancellation is not re-entrant through dispatch.
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
const EventFeed &Session::events() const noexcept { return impl_->events; }
const EpisodeRunner &Session::runner() const noexcept { return impl_->runner; }
EpisodeRunner &Session::runner() noexcept { return impl_->runner; }
std::size_t Session::tool_call_count() const noexcept {
  return impl_->events.size();
}

const std::filesystem::path &Session::metrics_path() const noexcept {
  return impl_->metrics->path();
}

const std::string &Session::metrics_error() const noexcept {
  return impl_->metrics_error;
}

} // namespace pigpen::agent
