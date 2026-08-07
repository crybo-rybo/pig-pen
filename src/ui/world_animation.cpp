#include "ui/world_animation.hpp"

#include <algorithm>
#include <string_view>

namespace pigpen::ui {
namespace {

[[nodiscard]] bool same_position(const world::Position left,
                                 const world::Position right) noexcept {
  return left == right;
}

} // namespace

void WorldAnimationState::reset(const world::Position position,
                                const std::size_t event_cursor) noexcept {
  pending_.clear();
  active_.reset();
  event_cursor_ = event_cursor;
  active_started_ = 0.0;
  last_update_ = 0.0;
  blob_ = {
      .x = static_cast<float>(position.x),
      .y = static_cast<float>(position.y),
  };
  initialized_ = true;
}

void WorldAnimationState::set_speed(const float speed) noexcept {
  speed_ = std::clamp(speed, 0.1F, 8.0F);
}

void WorldAnimationState::update(const agent::EventFeed &events,
                                 const world::Position world_position,
                                 const double now_seconds) {
  if (!initialized_) {
    reset(world_position);
  }
  if (events.size() < event_cursor_) {
    // A smaller feed means the owning Session was replaced. The application
    // normally calls reset explicitly; this keeps the renderer safe if it did
    // not get that notification.
    reset(world_position, events.size());
  }

  while (event_cursor_ < events.size()) {
    enqueue(events[event_cursor_]);
    ++event_cursor_;
  }

  last_update_ = now_seconds;
  if (!active_ && !pending_.empty()) {
    start_next(now_seconds);
  }

  while (active_) {
    const auto duration = active_duration();
    const auto elapsed = std::max(0.0, now_seconds - active_started_);
    const auto progress = std::clamp(elapsed / duration, 0.0, 1.0);

    if (active_->kind == StepKind::move) {
      const auto from_x = static_cast<double>(active_->before.x);
      const auto from_y = static_cast<double>(active_->before.y);
      blob_.x = static_cast<float>(
          from_x + (static_cast<double>(active_->after.x) - from_x) * progress);
      blob_.y = static_cast<float>(
          from_y + (static_cast<double>(active_->after.y) - from_y) * progress);
    }

    if (elapsed < duration) {
      return;
    }

    if (active_->kind == StepKind::move) {
      blob_ = {
          .x = static_cast<float>(active_->after.x),
          .y = static_cast<float>(active_->after.y),
      };
    }
    const auto next_started = active_started_ + duration;
    active_.reset();
    if (!pending_.empty()) {
      start_next(next_started);
    }
  }

  blob_ = {
      .x = static_cast<float>(world_position.x),
      .y = static_cast<float>(world_position.y),
  };
}

AnimatedPosition WorldAnimationState::blob_position() const noexcept {
  return blob_;
}

std::optional<VisualEffect>
WorldAnimationState::active_effect() const noexcept {
  if (!active_ || active_->kind == StepKind::move) {
    return std::nullopt;
  }
  return VisualEffect{
      .kind = active_->kind == StepKind::look ? VisualEffectKind::look
                                              : VisualEffectKind::eat,
      .origin = active_->before,
      .direction = active_->direction,
      .progress = active_progress(),
  };
}

std::size_t WorldAnimationState::queued_action_count() const noexcept {
  return pending_.size() + (active_ ? 1U : 0U);
}

void WorldAnimationState::enqueue(const agent::WorldEvent &event) {
  if (event.tool == "move") {
    if (!same_position(event.before, event.after)) {
      pending_.push_back({
          .kind = StepKind::move,
          .before = event.before,
          .after = event.after,
          .direction = event.direction,
      });
    }
    return;
  }
  if (event.tool == "look") {
    pending_.push_back({
        .kind = StepKind::look,
        .before = event.before,
        .after = event.after,
        .direction = event.direction,
    });
    return;
  }
  if (event.tool == "eat") {
    pending_.push_back({
        .kind = StepKind::eat,
        .before = event.before,
        .after = event.after,
        .direction = std::nullopt,
    });
  }
}

void WorldAnimationState::start_next(const double start_seconds) {
  active_ = pending_.front();
  pending_.pop_front();
  active_started_ = start_seconds;
  if (active_->kind == StepKind::move) {
    blob_ = {
        .x = static_cast<float>(active_->before.x),
        .y = static_cast<float>(active_->before.y),
    };
  }
}

double WorldAnimationState::active_duration() const noexcept {
  if (!active_) {
    return 0.001;
  }
  const auto base_duration = active_->kind == StepKind::move ? 0.150 : 0.230;
  return base_duration / static_cast<double>(speed_);
}

float WorldAnimationState::active_progress() const noexcept {
  if (!active_) {
    return 0.0F;
  }
  const auto elapsed = std::max(0.0, last_update_ - active_started_);
  return static_cast<float>(std::clamp(elapsed / active_duration(), 0.0, 1.0));
}

} // namespace pigpen::ui
