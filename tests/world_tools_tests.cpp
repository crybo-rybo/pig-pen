/// @file world_tools_tests.cpp
/// @brief Covers typed world results and the opaque_look / reward_feedback
/// visibility toggles.
///
/// WorldTools takes world values and returns the plain response types, so
/// everything here runs without reflection, JSON, a scry registry, or a model.

#include "core/world_tools.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace {

using pigpen::core::Config;
using pigpen::core::WorldTools;
using pigpen::world::Direction;
using pigpen::world::Position;
using pigpen::world::World;

/// @brief Walks the blob to @p destination one legal step at a time,
/// asserting every intermediate move succeeds.
void move_to(World &world, const Position destination) {
  while (world.position().x < destination.x) {
    REQUIRE(world.move(Direction::east).ok);
  }
  while (world.position().x > destination.x) {
    REQUIRE(world.move(Direction::west).ok);
  }
  while (world.position().y < destination.y) {
    REQUIRE(world.move(Direction::north).ok);
  }
  while (world.position().y > destination.y) {
    REQUIRE(world.move(Direction::south).ok);
  }
}

/// @brief A cell adjacent to an item plus the direction that looks at it.
struct ItemViewpoint {
  Position position{};
  Direction direction{};
};

/// @brief Picks an in-bounds cell next to @p item from which a single look
/// puts the item in the ray's first cell.
[[nodiscard]] ItemViewpoint viewpoint_for(const Position item) {
  if (item.x > 0) {
    return {
        .position = {.x = item.x - 1, .y = item.y},
        .direction = Direction::east,
    };
  }
  return {
      .position = {.x = item.x + 1, .y = item.y},
      .direction = Direction::west,
  };
}

} // namespace

TEST_CASE("Move returns a fixed typed result for success and wall failure") {
  World world{37};
  WorldTools tools{world};

  const auto east = tools.move(Direction::east);
  CHECK(east.ok);
  CHECK(east.position == (Position{.x = 6, .y = 5}));

  for (int y = world.position().y; y < World::height - 1; ++y) {
    const auto moved = tools.move(Direction::north);
    REQUIRE(moved.ok);
  }
  const auto before_wall = world.position();
  const auto wall = tools.move(Direction::north);
  CHECK_FALSE(wall.ok);
  REQUIRE(wall.reason);
  CHECK(*wall.reason == pigpen::world::MoveFailure::wall);
  CHECK(wall.position == before_wall);
  CHECK(world.position() == before_wall);
}

TEST_CASE("Look returns a typed complete ray and supports opaque items") {
  constexpr std::uint64_t seed = 73;
  World visible_world{seed};
  const auto placement = visible_world.items().front();
  const auto viewpoint = viewpoint_for(placement.position);
  move_to(visible_world, viewpoint.position);
  WorldTools visible_tools{visible_world};

  const auto visible = visible_tools.look(viewpoint.direction);
  REQUIRE(visible.ok);
  REQUIRE_FALSE(visible.cells.empty());
  REQUIRE(visible.cells.front().item);
  CHECK(*visible.cells.front().item ==
        pigpen::world::item_name(placement.item));
  CHECK(visible.wall_at_distance == static_cast<int>(visible.cells.size()) + 1);

  World opaque_world{seed};
  move_to(opaque_world, viewpoint.position);
  Config config;
  config.opaque_look = true;
  WorldTools opaque_tools{opaque_world, config};
  const auto opaque = opaque_tools.look(viewpoint.direction);
  REQUIRE(opaque.ok);
  REQUIRE(opaque.cells.front().item);
  CHECK(*opaque.cells.front().item == "something");
}

TEST_CASE(
    "Eat returns typed truth while reward visibility stays configurable") {
  constexpr std::uint64_t seed = 808;
  World feedback_world{seed};
  const auto placement = feedback_world.items().front();
  move_to(feedback_world, placement.position);
  WorldTools feedback_tools{feedback_world};

  const auto revealed = feedback_tools.eat();
  REQUIRE(revealed.ok);
  CHECK(revealed.ate == placement.item);
  CHECK(revealed.reward == pigpen::world::item_reward(placement.item));
  CHECK(revealed.score == pigpen::world::item_reward(placement.item));

  World hidden_world{seed};
  move_to(hidden_world, placement.position);
  Config config;
  config.reward_feedback = false;
  WorldTools hidden_tools{hidden_world, config};
  const auto hidden = hidden_tools.eat();
  REQUIRE(hidden.ok);
  CHECK_FALSE(hidden.reward);
  CHECK_FALSE(hidden.score);
  CHECK(hidden.ate == placement.item);
  CHECK(hidden_world.score() == pigpen::world::item_reward(placement.item));
}
