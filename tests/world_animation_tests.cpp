/// @file world_animation_tests.cpp
/// @brief Covers the activity feed becoming an ordered visual timeline: burst
/// moves play back sequentially, look/eat become transient effects, and
/// wall-rejected calls never animate.
///
/// WorldAnimationState takes the current time as a parameter, so these cases
/// step a fake clock instead of needing ImGui or a real frame loop.

#include "ui/world_animation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

/// @brief Builds a successful eastward move, varying only the animation fields.
[[nodiscard]] pigpen::core::ToolActivity
move_activity(const std::uint64_t tick, const pigpen::world::Position before,
              const pigpen::world::Position after) {
  return {
      .tick = tick,
      .turn = 1,
      .kind = pigpen::core::ToolKind::move,
      .outcome = pigpen::core::ToolOutcome::succeeded,
      .arguments_json = R"({"direction":"east"})",
      .before = before,
      .after = after,
      .direction = pigpen::world::Direction::east,
  };
}

} // namespace

TEST_CASE("burst moves animate sequentially instead of teleporting") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 5, .y = 5});
  const pigpen::core::ToolActivityFeed activities{
      move_activity(1, {.x = 5, .y = 5}, {.x = 6, .y = 5}),
      move_activity(2, {.x = 6, .y = 5}, {.x = 7, .y = 5}),
  };

  animation.update(activities, {.x = 7, .y = 5}, 10.0);
  CHECK(animation.queued_action_count() == 2);
  CHECK(animation.blob_position().x == Catch::Approx(5.0F));

  animation.update(activities, {.x = 7, .y = 5}, 10.075);
  CHECK(animation.blob_position().x == Catch::Approx(5.5F));

  animation.update(activities, {.x = 7, .y = 5}, 10.150);
  CHECK(animation.queued_action_count() == 1);
  CHECK(animation.blob_position().x == Catch::Approx(6.0F));

  animation.update(activities, {.x = 7, .y = 5}, 10.225);
  CHECK(animation.blob_position().x == Catch::Approx(6.5F));

  animation.update(activities, {.x = 7, .y = 5}, 10.300);
  CHECK(animation.queued_action_count() == 0);
  CHECK(animation.blob_position().x == Catch::Approx(7.0F));
}

TEST_CASE("look and eat activities become ordered transient effects") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 5, .y = 5});
  const pigpen::core::ToolActivityFeed activities{
      {
          .tick = 1,
          .turn = 1,
          .kind = pigpen::core::ToolKind::look,
          .outcome = pigpen::core::ToolOutcome::succeeded,
          .arguments_json = R"({"direction":"north"})",
          .before = {.x = 5, .y = 5},
          .after = {.x = 5, .y = 5},
          .direction = pigpen::world::Direction::north,
      },
      {
          .tick = 2,
          .turn = 1,
          .kind = pigpen::core::ToolKind::eat,
          .outcome = pigpen::core::ToolOutcome::succeeded,
          .before = {.x = 5, .y = 5},
          .after = {.x = 5, .y = 5},
      },
  };

  animation.update(activities, {.x = 5, .y = 5}, 2.0);
  auto effect = animation.active_effect();
  REQUIRE(effect.has_value());
  CHECK(effect->kind == pigpen::core::ToolKind::look);
  REQUIRE(effect->direction.has_value());
  CHECK(*effect->direction == pigpen::world::Direction::north);

  animation.update(activities, {.x = 5, .y = 5}, 2.231);
  effect = animation.active_effect();
  REQUIRE(effect.has_value());
  CHECK(effect->kind == pigpen::core::ToolKind::eat);

  animation.update(activities, {.x = 5, .y = 5}, 2.461);
  CHECK_FALSE(animation.active_effect().has_value());
  CHECK(animation.queued_action_count() == 0);
}

TEST_CASE("animation speed changes move duration") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 1, .y = 1});
  animation.set_speed(2.0F);
  const pigpen::core::ToolActivityFeed activities{
      move_activity(1, {.x = 1, .y = 1}, {.x = 2, .y = 1}),
  };

  animation.update(activities, {.x = 2, .y = 1}, 3.0);
  animation.update(activities, {.x = 2, .y = 1}, 3.075);
  CHECK(animation.queued_action_count() == 0);
  CHECK(animation.blob_position().x == Catch::Approx(2.0F));
}

TEST_CASE("wall-blocked moves do not animate movement") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 5, .y = 5});
  const pigpen::core::ToolActivityFeed activities{{
      .tick = 1,
      .turn = 1,
      .kind = pigpen::core::ToolKind::move,
      .outcome = pigpen::core::ToolOutcome::blocked_by_wall,
      .arguments_json = R"({"direction":"north"})",
      .before = {.x = 5, .y = 5},
      .after = {.x = 5, .y = 5},
      .direction = pigpen::world::Direction::north,
  }};

  animation.update(activities, {.x = 5, .y = 5}, 4.0);
  CHECK(animation.queued_action_count() == 0);
  CHECK_FALSE(animation.active_effect());
  CHECK(animation.blob_position().x == Catch::Approx(5.0F));
  CHECK(animation.blob_position().y == Catch::Approx(5.0F));
}
