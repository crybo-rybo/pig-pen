#include "ui/world_animation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

namespace {

[[nodiscard]] pigpen::agent::WorldEvent
move_event(const std::uint64_t tick, const pigpen::world::Position before,
           const pigpen::world::Position after) {
  return {
      .tick = tick,
      .turn = 1,
      .tool = "move",
      .arguments = {{"direction", "east"}},
      .result = {{"ok", true}},
      .before = before,
      .after = after,
      .direction = pigpen::world::Direction::east,
  };
}

} // namespace

TEST_CASE("burst moves animate sequentially instead of teleporting") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 5, .y = 5});
  const pigpen::agent::EventFeed events{
      move_event(1, {.x = 5, .y = 5}, {.x = 6, .y = 5}),
      move_event(2, {.x = 6, .y = 5}, {.x = 7, .y = 5}),
  };

  animation.update(events, {.x = 7, .y = 5}, 10.0);
  CHECK(animation.queued_action_count() == 2);
  CHECK(animation.blob_position().x == Catch::Approx(5.0F));

  animation.update(events, {.x = 7, .y = 5}, 10.075);
  CHECK(animation.blob_position().x == Catch::Approx(5.5F));

  animation.update(events, {.x = 7, .y = 5}, 10.150);
  CHECK(animation.queued_action_count() == 1);
  CHECK(animation.blob_position().x == Catch::Approx(6.0F));

  animation.update(events, {.x = 7, .y = 5}, 10.225);
  CHECK(animation.blob_position().x == Catch::Approx(6.5F));

  animation.update(events, {.x = 7, .y = 5}, 10.300);
  CHECK(animation.queued_action_count() == 0);
  CHECK(animation.blob_position().x == Catch::Approx(7.0F));
}

TEST_CASE("look and eat events become ordered transient effects") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 5, .y = 5});
  const pigpen::agent::EventFeed events{
      {
          .tick = 1,
          .turn = 1,
          .tool = "look",
          .arguments = {{"direction", "north"}},
          .result = {{"direction", "north"}},
          .before = {.x = 5, .y = 5},
          .after = {.x = 5, .y = 5},
          .direction = pigpen::world::Direction::north,
      },
      {
          .tick = 2,
          .turn = 1,
          .tool = "eat",
          .arguments = nlohmann::json::object(),
          .result = {{"ok", false}},
          .before = {.x = 5, .y = 5},
          .after = {.x = 5, .y = 5},
      },
  };

  animation.update(events, {.x = 5, .y = 5}, 2.0);
  auto effect = animation.active_effect();
  REQUIRE(effect.has_value());
  CHECK(effect->kind == pigpen::ui::VisualEffectKind::look);
  REQUIRE(effect->direction.has_value());
  CHECK(*effect->direction == pigpen::world::Direction::north);

  animation.update(events, {.x = 5, .y = 5}, 2.231);
  effect = animation.active_effect();
  REQUIRE(effect.has_value());
  CHECK(effect->kind == pigpen::ui::VisualEffectKind::eat);

  animation.update(events, {.x = 5, .y = 5}, 2.461);
  CHECK_FALSE(animation.active_effect().has_value());
  CHECK(animation.queued_action_count() == 0);
}

TEST_CASE("animation speed changes move duration") {
  pigpen::ui::WorldAnimationState animation;
  animation.reset({.x = 1, .y = 1});
  animation.set_speed(2.0F);
  const pigpen::agent::EventFeed events{
      move_event(1, {.x = 1, .y = 1}, {.x = 2, .y = 1}),
  };

  animation.update(events, {.x = 2, .y = 1}, 3.0);
  animation.update(events, {.x = 2, .y = 1}, 3.075);
  CHECK(animation.queued_action_count() == 0);
  CHECK(animation.blob_position().x == Catch::Approx(2.0F));
}
