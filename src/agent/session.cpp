/// @file session.cpp
/// @brief Session implementation: config validation, composition, and
/// reflection-based tool registration; the contract is in the header.
#include "agent/session.hpp"

#include "agent/metrics_writer.hpp"
#include "agent/prompt.hpp"
#include "agent/scry_transport.hpp"
#include "agent/world_tool_controller.hpp"
#include "agent/world_tools.hpp"

#include <scry/reflection.hpp>
#include <scry/scry.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
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

/// @brief Build the scry harness from a validated Config, with
/// PIGPEN_API_KEY from the environment as the credential.
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
      : config(std::move(initial_config)), world(this->config.seed),
        metrics(std::move(initial_metrics)),
        harness(std::move(initial_harness)),
        conversation(std::move(initial_conversation)),
        tools(world, this->config),
        journal([this](const ToolActivity &activity) {
          return metrics->record_tool(activity);
        }),
        transport(this->harness, this->conversation),
        runner(
            transport, static_cast<std::uint32_t>(this->config.turn_budget),
            [this] { return world.all_positive_items_eaten(); },
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
                              this->metrics->finish(result, world.score());
                          !status) {
                        metrics_error = std::move(status.error());
                      }
                    },
            },
            [this] { return journal.call_count(); }),
        controller(tools, journal,
                   [this] { return runner.snapshot().turns_used + 1U; }) {}

  // Members hold this-capturing lambdas and references to their siblings;
  // an Impl must never be copied or moved.
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  Config config;
  world::World world;
  std::unique_ptr<MetricsWriter> metrics;
  scry::Harness harness;
  scry::Conversation conversation;
  WorldTools tools;
  ToolActivityJournal journal;
  ScryTurnTransport transport;
  EpisodeRunner runner;
  WorldToolController controller;
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
  // Registration is metadata plus a thin typed callable: decode belongs to
  // scry, and everything after decode belongs to WorldToolController.
  const auto add = [this, weak_session]<typename Arguments, typename Response>(
                       scry::reflection::ToolMetadata metadata,
                       Response (WorldToolController::*handler)(Arguments)) {
    return scry::reflection::add<Arguments>(
        impl_->harness.tools(), std::move(metadata),
        [weak_session, handler](Arguments arguments) -> scry::Result<Response> {
          const auto session = weak_session.lock();
          if (!session) {
            return std::unexpected(scry::Error{
                .category = scry::ErrorCategory::invalid_state,
                .message = "pig-pen session no longer exists",
            });
          }
          return std::invoke(handler, session->impl_->controller,
                             std::move(arguments));
        });
  };

  if (auto status = add(
          {
              .name = "move",
              .description = "Move one cell north, south, east, or west.",
          },
          &WorldToolController::move);
      !status) {
    return std::unexpected(status.error().message);
  }
  if (auto status = add(
          {
              .name = "look",
              .description = "Scan every cell in one direction to the wall.",
          },
          &WorldToolController::look);
      !status) {
    return std::unexpected(status.error().message);
  }
  if (auto status = add(
          {
              .name = "eat",
              .description = "Eat the item on the current cell, if present.",
          },
          &WorldToolController::eat);
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
  if (impl_->journal.take_persistence_failure()) {
    // The world action and its typed response have already committed. Fail the
    // episode only after Harness::update returns so the model receives that
    // truthful response and cancellation is not re-entrant through dispatch.
    impl_->metrics_error = impl_->journal.persistence_error();
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
const world::World &Session::world() const noexcept { return impl_->world; }

const ToolActivityJournal &Session::activity_journal() const noexcept {
  return impl_->journal;
}

const EpisodeRunner &Session::runner() const noexcept { return impl_->runner; }
EpisodeRunner &Session::runner() noexcept { return impl_->runner; }

const std::filesystem::path &Session::metrics_path() const noexcept {
  return impl_->metrics->path();
}

const std::string &Session::metrics_error() const noexcept {
  return impl_->metrics_error.empty() ? impl_->journal.persistence_error()
                                      : impl_->metrics_error;
}

} // namespace pigpen::agent
