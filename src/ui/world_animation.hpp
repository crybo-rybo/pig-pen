#pragma once

#include "agent/events.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace pigpen::ui {

struct AnimatedPosition {
  float x{};
  float y{};
};

enum class VisualEffectKind {
  look,
  eat,
};

struct VisualEffect {
  VisualEffectKind kind{VisualEffectKind::look};
  world::Position origin{};
  std::optional<world::Direction> direction{};
  float progress{};
};

/// Converts the append-only world event feed into a deterministic visual
/// timeline. Time is supplied by the caller so the state can be tested without
/// a window or wall-clock sleeps.
class WorldAnimationState final {
public:
  void reset(world::Position position, std::size_t event_cursor = 0U) noexcept;
  void set_speed(float speed) noexcept;

  void update(const agent::EventFeed &events, world::Position world_position,
              double now_seconds);

  [[nodiscard]] AnimatedPosition blob_position() const noexcept;
  [[nodiscard]] std::optional<VisualEffect> active_effect() const noexcept;
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

  void enqueue(const agent::WorldEvent &event);
  void start_next(double start_seconds);
  [[nodiscard]] double active_duration() const noexcept;
  [[nodiscard]] float active_progress() const noexcept;

  std::deque<Step> pending_{};
  std::optional<Step> active_{};
  std::size_t event_cursor_{};
  double active_started_{};
  double last_update_{};
  float speed_{1.0F};
  AnimatedPosition blob_{};
  bool initialized_{false};
};

} // namespace pigpen::ui
