/// @file session.hpp
/// @brief Session: the composition root and atomic reset unit.
///
/// A Session owns the world, conversation, scry harness with its registered
/// tools, episode runner, and metrics writer. There is no partial reset —
/// starting over means destroying the session and creating a new one.
#pragma once

#include "agent/config.hpp"
#include "agent/episode_runner.hpp"
#include "agent/events.hpp"
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

/// @brief One complete episode runtime. Replacing this object atomically
/// resets the world, conversation, additive tool registry, callbacks, and
/// log.
///
/// Tools are registered on the harness with a weak_ptr back to the session,
/// so a callback arriving after the session is gone fails cleanly instead
/// of touching freed state.
class Session final : public std::enable_shared_from_this<Session> {
public:
  /// @brief Validate @p config, open the metrics log, build the scry
  /// harness and conversation, and register the world tools.
  /// @return The session, or a human-readable rejection message.
  [[nodiscard]] static std::expected<std::shared_ptr<Session>, std::string>
  create(Config config, std::filesystem::path log_directory = "logs",
         std::string prompt_variant = "default");

  ~Session();

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  /// @brief Drive the harness, then tick the runner. Call from the front
  /// end's own loop — the GUI once per frame, the CLI in a tight loop.
  /// @note The harness gets a 2 ms time budget and at most 32 callbacks per
  /// pump, so no single pass can stall a frame.
  [[nodiscard]] PumpStats pump();
  /// @brief Forwards to EpisodeRunner::play().
  [[nodiscard]] bool play();
  /// @brief Forwards to EpisodeRunner::pause().
  [[nodiscard]] bool pause();
  /// @brief Forwards to EpisodeRunner::stop(); cancellation is cooperative.
  [[nodiscard]] bool stop();
  /// @brief Forwards to EpisodeRunner::queue_user_input().
  [[nodiscard]] std::uint64_t queue_user_input(std::string message);
  /// @brief Forwards to EpisodeRunner::remove_pending_user_input().
  [[nodiscard]] bool remove_pending_user_input(std::uint64_t id);
  /// @brief Forwards to EpisodeRunner::clear_pending_user_inputs().
  void clear_pending_user_inputs();

  /// @brief The validated configuration this session was created with.
  [[nodiscard]] const Config &config() const noexcept;
  /// @brief The simulation; mutated only through registered tools.
  [[nodiscard]] const world::World &world() const noexcept;
  /// @brief Append-only feed of successfully decoded tool invocations.
  [[nodiscard]] const EventFeed &events() const noexcept;
  /// @brief The episode state machine.
  [[nodiscard]] const EpisodeRunner &runner() const noexcept;
  /// @brief The episode state machine.
  [[nodiscard]] EpisodeRunner &runner() noexcept;
  /// @brief Number of successfully decoded world-tool invocations so far.
  [[nodiscard]] std::size_t tool_call_count() const noexcept;
  /// @brief Path of this session's JSONL metrics log.
  [[nodiscard]] const std::filesystem::path &metrics_path() const noexcept;
  /// @brief Last metrics-write failure, or empty; a failure is terminal.
  [[nodiscard]] const std::string &metrics_error() const noexcept;

private:
  class Impl;

  explicit Session(std::unique_ptr<Impl> impl);
  [[nodiscard]] std::expected<void, std::string> register_tools();

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::agent
