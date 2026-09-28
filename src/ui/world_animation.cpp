/// @file world_animation.cpp
/// @brief Implements the activity-feed-to-visual-timeline state machine.
#include "ui/world_animation.hpp"

#include <algorithm>
#include <cmath>

namespace pigpen::ui {

void WorldAnimationState::reset(const world::Position position) noexcept {
  pending_.clear();
  active_.reset();
  activity_cursor_ = 0;
  progress_ = 0.0F;
  blob_ = {.x = static_cast<float>(position.x),
           .y = static_cast<float>(position.y)};
}

void WorldAnimationState::set_speed(const float speed) noexcept {
  speed_ = std::clamp(speed, 0.1F, 8.0F);
}

void WorldAnimationState::update(const agent::ToolActivityFeed &activities,
                                 const world::Position world_position,
                                 const double now_seconds) {
  for (; activity_cursor_ < activities.size(); ++activity_cursor_) {
    const auto &activity = activities[activity_cursor_];
    // A wall-blocked move has nothing to show.
    if (activity.kind != agent::ToolKind::move ||
        activity.before != activity.after) {
      pending_.push_back({.kind = activity.kind,
                          .before = activity.before,
                          .after = activity.after,
                          .direction = activity.direction});
    }
  }

  // Each finished step hands its exact end time to the next, so a long frame
  // skips through several steps without accumulating drift.
  auto step_started = now_seconds;
  while (active_ || !pending_.empty()) {
    if (!active_) {
      active_ = pending_.front();
      pending_.pop_front();
      active_started_ = step_started;
    }
    const auto duration =
        (active_->kind == agent::ToolKind::move ? 0.150 : 0.230) /
        static_cast<double>(speed_);
    const auto elapsed = now_seconds - active_started_;
    if (elapsed < duration) {
      progress_ = static_cast<float>(std::max(0.0, elapsed) / duration);
      blob_ = {.x = std::lerp(static_cast<float>(active_->before.x),
                              static_cast<float>(active_->after.x), progress_),
               .y = std::lerp(static_cast<float>(active_->before.y),
                              static_cast<float>(active_->after.y), progress_)};
      return;
    }
    step_started = active_started_ + duration;
    active_.reset();
  }

  blob_ = {.x = static_cast<float>(world_position.x),
           .y = static_cast<float>(world_position.y)};
}

std::optional<VisualEffect>
WorldAnimationState::active_effect() const noexcept {
  if (!active_ || active_->kind == agent::ToolKind::move) {
    return std::nullopt;
  }
  return VisualEffect{.kind = active_->kind,
                      .origin = active_->before,
                      .direction = active_->direction,
                      .progress = progress_};
}

} // namespace pigpen::ui
