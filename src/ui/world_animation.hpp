/// @file world_animation.hpp
/// @brief Converts the tool-activity feed into a played-back visual timeline.
#pragma once

#include "agent/events.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace pigpen::ui {

/// @brief The blob's interpolated draw position, in fractional grid cells.
struct AnimatedPosition {
  float x{};
  float y{};
};

/// @brief A look or eat action currently being played back.
struct VisualEffect {
  agent::ToolKind kind{agent::ToolKind::look};
  world::Position origin{};
  /// Set for look effects; eat has no direction.
  std::optional<world::Direction> direction{};
  /// Playback progress in [0, 1].
  float progress{};
};

/// @brief Replays the append-only activity feed one timed step at a time, so
/// a burst of tool calls reads as a sequence rather than a teleport.
///
/// Time is supplied by the caller, which keeps the type free of ImGui and
/// wall-clock dependencies and lets the tests drive it directly.
class WorldAnimationState final {
public:
  /// @brief Adopts a fresh session: drops all steps, restarts consumption at
  /// the head of its (empty) feed, and snaps the blob to @p position.
  void reset(world::Position position) noexcept;

  /// @brief Sets the playback speed multiplier, clamped to a sane range.
  void set_speed(float speed) noexcept;

  /// @brief Consumes new activities and advances playback to @p now_seconds.
  /// @param world_position Authoritative blob position, adopted once the
  /// queue drains so animation can never drift from the simulation.
  void update(const agent::ToolActivityFeed &activities,
              world::Position world_position, double now_seconds);

  /// @brief Where to draw the blob this frame.
  [[nodiscard]] AnimatedPosition blob_position() const noexcept {
    return blob_;
  }
  /// @brief The look/eat effect in progress, if any; moves report nothing.
  [[nodiscard]] std::optional<VisualEffect> active_effect() const noexcept;
  /// @brief Steps not yet finished playing, including the active one.
  [[nodiscard]] std::size_t queued_action_count() const noexcept {
    return pending_.size() + (active_ ? 1U : 0U);
  }

private:
  struct Step {
    agent::ToolKind kind{agent::ToolKind::move};
    world::Position before{};
    world::Position after{};
    std::optional<world::Direction> direction{};
  };

  std::deque<Step> pending_{};
  std::optional<Step> active_{};
  std::size_t activity_cursor_{};
  double active_started_{};
  float progress_{};
  float speed_{1.0F};
  AnimatedPosition blob_{};
};

} // namespace pigpen::ui
