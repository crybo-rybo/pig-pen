/// @file world_animation.hpp
/// @brief Converts the tool-activity feed into a played-back visual timeline.
///
/// A turn can produce a burst of tool calls at once; this state machine
/// replays them one timed step at a time so the burst reads as a sequence
/// rather than a teleport. Time is a caller-supplied parameter, which keeps
/// the type free of ImGui and wall-clock dependencies and lets the tests
/// drive it directly.
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

/// @brief Which non-movement action the renderer should visualize.
enum class VisualEffectKind {
  look,
  eat,
};

/// @brief A look or eat action currently being played back.
struct VisualEffect {
  VisualEffectKind kind{VisualEffectKind::look};
  world::Position origin{};
  /// Set for look effects; eat has no direction.
  std::optional<world::Direction> direction{};
  /// Playback progress in [0, 1].
  float progress{};
};

/// @brief Converts the append-only tool-activity feed into a deterministic
/// visual timeline.
///
/// Time is supplied by the caller so the state can be tested without a
/// window or wall-clock sleeps.
class WorldAnimationState final {
public:
  /// @brief Drops all queued steps and snaps the blob to @p position.
  /// @param activity_cursor Feed index to resume consuming from; pass the feed
  /// size when adopting a fresh session so old activities are not replayed.
  void reset(world::Position position,
             std::size_t activity_cursor = 0U) noexcept;

  /// @brief Sets the playback speed multiplier, clamped to a sane range.
  void set_speed(float speed) noexcept;

  /// @brief Consumes new events and advances the active step.
  /// @param world_position Authoritative blob position, adopted once the
  /// queue drains so animation can never drift from the simulation.
  /// @param now_seconds Caller-supplied monotonic time.
  void update(const agent::ToolActivityFeed &activities,
              world::Position world_position, double now_seconds);

  /// @brief Where to draw the blob this frame.
  [[nodiscard]] AnimatedPosition blob_position() const noexcept;
  /// @brief The look/eat effect in progress, if any; moves report nothing.
  [[nodiscard]] std::optional<VisualEffect> active_effect() const noexcept;
  /// @brief Steps not yet finished playing, including the active one.
  [[nodiscard]] std::size_t queued_action_count() const noexcept;

private:
  enum class StepKind {
    move,
    look,
    eat,
  };

  struct Step {
    StepKind kind{StepKind::move};
    world::Position before{};
    world::Position after{};
    std::optional<world::Direction> direction{};
  };

  void enqueue(const agent::ToolActivity &activity);
  void start_next(double start_seconds);
  [[nodiscard]] double active_duration() const noexcept;
  [[nodiscard]] float active_progress() const noexcept;

  std::deque<Step> pending_{};
  std::optional<Step> active_{};
  std::size_t activity_cursor_{};
  double active_started_{};
  double last_update_{};
  float speed_{1.0F};
  AnimatedPosition blob_{};
  bool initialized_{false};
};

} // namespace pigpen::ui
