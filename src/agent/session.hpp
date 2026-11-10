/// @file session.hpp
/// @brief Session: the composition root and atomic reset unit.
///
/// A Session owns the world, the llamad connection, the tool loop with its
/// conversation, the bound world tools, the episode runner, and the metrics
/// writer. There is no partial reset — starting over means destroying the
/// session and creating a new one.
#pragma once

#include "core/chat_client.hpp"
#include "core/config.hpp"
#include "core/episode_runner.hpp"
#include "core/events.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace pigpen::agent {

/// @brief What one pump() pass accomplished; both front ends surface this.
struct PumpStats {
  std::size_t callbacks_delivered{};
  std::size_t events_remaining{};
};

/// @brief Token counts of all finished turns of the episode.
struct TokenTotals {
  std::uint64_t input{};
  std::uint64_t output{};
  /// Input tokens that llamad reused from its KV cache.
  std::uint64_t cached_input{};
};

/// @brief One complete episode runtime. Replacing this object atomically
/// resets the world, the conversation, the tools, the callbacks, and the
/// log.
///
/// The tool handlers share ownership of the bindings; the world outlives
/// them, and model delivery is cancelled before any state is destroyed.
class Session final {
public:
  /// @brief Validate @p config, connect to llamad at Config::socket_path,
  /// and compose the world, tools, tool loop, and an already-open metrics
  /// log.
  /// @return The session, or a human-readable rejection message. A rejected
  /// config or an unreachable daemon leaves no log behind.
  [[nodiscard]] static std::expected<std::shared_ptr<Session>, std::string>
  create(core::Config config, std::filesystem::path log_directory = "logs",
         std::string prompt_variant = "default");

  ~Session();

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  /// @brief Deliver queued model events, then tick the runner. Call from the
  /// front end's own loop — the GUI once per frame, the CLI in a tight loop.
  /// @note At most 32 callbacks are delivered per pump, so no single pass
  /// can stall a frame. Tools run inside this call.
  [[nodiscard]] PumpStats pump();

  // Forward to the EpisodeRunner; stop() cancellation is cooperative.
  [[nodiscard]] bool play();
  [[nodiscard]] bool pause();
  [[nodiscard]] bool stop();
  [[nodiscard]] std::uint64_t queue_user_input(std::string message);
  [[nodiscard]] bool remove_pending_user_input(std::uint64_t id);
  void clear_pending_user_inputs();

  [[nodiscard]] const core::Config &config() const noexcept;
  /// @brief The model that the daemon serves, as it reported at connect.
  [[nodiscard]] const core::ModelInfo &model_info() const noexcept;
  /// @brief The simulation; mutated only through the world tools.
  [[nodiscard]] const world::World &world() const noexcept;
  /// @brief Append-only feed of successfully decoded tool activity.
  [[nodiscard]] const core::ToolActivityFeed &tool_activities() const noexcept;
  [[nodiscard]] const core::EpisodeRunner &runner() const noexcept;
  [[nodiscard]] const TokenTotals &token_totals() const noexcept;
  [[nodiscard]] const std::filesystem::path &metrics_path() const noexcept;
  /// @brief Last metrics-write failure, or empty; a failure is terminal.
  [[nodiscard]] const std::string &metrics_error() const noexcept;

private:
  struct Impl;

  explicit Session(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::agent
