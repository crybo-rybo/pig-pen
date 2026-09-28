/// @file session.hpp
/// @brief Session: the composition root and atomic reset unit.
///
/// A Session owns the world, conversation, scry harness with its registered
/// tools, episode runner, and (when asked for) metrics writer. There is no
/// partial reset — starting over means destroying the session and creating
/// a new one.
#pragma once

#include "agent/config.hpp"
#include "agent/episode_driver.hpp"
#include "agent/episode_runner.hpp"
#include "agent/episode_turn.hpp"
#include "agent/events.hpp"
#include "agent/reward.hpp"
#include "agent/session_options.hpp"
#include "world/world.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// @brief One complete episode runtime. Replacing this object atomically
/// resets the world, conversation, additive tool registry, callbacks, and
/// log.
///
/// The harness adopts a standalone tool registry whose bindings and world
/// outlive it; turn delivery is disconnected before any state is destroyed.
class Session final : public IDrivableEpisode {
public:
  /// @brief Validate @p config and @p options, then compose the world,
  /// registered harness, conversation, and, when options.log_directory is
  /// set, an already-open metrics log.
  /// @return The session, or a human-readable rejection message. A rejected
  /// configuration (including a bad request header) opens no log.
  [[nodiscard]] static std::expected<std::shared_ptr<Session>, std::string>
  create(Config config, SessionOptions options = {});

  ~Session() override;

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  /// @brief Drive the harness, then tick the runner. Call from the front
  /// end's own loop — the GUI once per frame, the CLI in a tight loop.
  /// @note The harness gets a 2 ms time budget and at most 32 callbacks per
  /// pump, so no single pass can stall a frame.
  [[nodiscard]] PumpStats pump() override;
  /// @brief The runner has reached RunState::finished.
  [[nodiscard]] bool finished() const override;

  // Forward to the EpisodeRunner; stop() cancellation is cooperative.
  [[nodiscard]] bool play();
  [[nodiscard]] bool pause();
  [[nodiscard]] bool stop() override;
  [[nodiscard]] std::uint64_t queue_user_input(std::string message);
  [[nodiscard]] bool remove_pending_user_input(std::uint64_t id);
  void clear_pending_user_inputs();

  [[nodiscard]] const Config &config() const noexcept;
  /// @brief The simulation; mutated only through registered tools.
  [[nodiscard]] const world::World &world() const noexcept;
  /// @brief Append-only feed of successfully decoded tool activity.
  [[nodiscard]] const ToolActivityFeed &tool_activities() const noexcept;
  [[nodiscard]] const EpisodeRunner &runner() const noexcept;
  /// @brief Every turn the runner finished, oldest first, each with its call
  /// tally. A turn the runner abandoned in flight (fail()) is not retained.
  [[nodiscard]] const std::vector<EpisodeTurn> &turns() const noexcept;
  /// @brief Wall time since the session was created, on a steady clock;
  /// frozen once the episode finishes, so it is then the episode duration.
  [[nodiscard]] std::chrono::milliseconds elapsed() const;
  /// @brief SessionOptions::rollout_id from create(), or empty.
  [[nodiscard]] const std::string &rollout_id() const noexcept;
  /// @brief SessionOptions::reward_weights from create(), used for the
  /// footer's reward.
  [[nodiscard]] const RewardWeights &reward_weights() const noexcept;
  /// @brief The open log's path, or nullopt when the session writes none.
  [[nodiscard]] const std::optional<std::filesystem::path> &
  metrics_path() const noexcept;
  /// @brief Last metrics-write failure, or empty; a failure is terminal.
  /// Always empty without a log.
  [[nodiscard]] const std::string &metrics_error() const noexcept;

private:
  struct Impl;

  explicit Session(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::agent
