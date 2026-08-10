/// @file world_tools_tests.cpp
/// @brief Covers the reflected tool boundary: compile-time input schemas,
/// typed result envelopes, the explicit begin_turn/budget lifecycle, and the
/// opaque_look / reward_feedback visibility toggles.
///
/// WorldTools accepts and returns only reflected C++ values, so everything
/// here runs without JSON parsing, a scry registry, or a model.

#include "agent/world_tools.hpp"

#include "agent/tool_contract.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <scry/reflection.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using pigpen::agent::Config;
using pigpen::agent::DirectionArguments;
using pigpen::agent::EatArguments;
using pigpen::agent::ToolFailureCode;
using pigpen::agent::WorldTools;
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

static_assert(
    scry::reflection::input_schema_v<DirectionArguments> ==
    R"({"additionalProperties":false,"properties":{"direction":{"description":"Cardinal direction: north, south, east, or west","enum":["north","south","east","west"],"type":"string"}},"required":["direction"],"type":"object"})");
static_assert(
    scry::reflection::input_schema_v<EatArguments> ==
    R"({"additionalProperties":false,"properties":{},"required":[],"type":"object"})");

TEST_CASE("C++ declarations are the complete model input-schema source") {
  const auto directional = nlohmann::json::parse(
      scry::reflection::input_schema_v<DirectionArguments>);
  CHECK(directional.at("type") == "object");
  CHECK(directional.at("additionalProperties") == false);
  CHECK(directional.at("required") == nlohmann::json::array({"direction"}));
  CHECK(directional.at("properties").at("direction").at("enum") ==
        nlohmann::json::array({"north", "south", "east", "west"}));
  CHECK(directional.at("properties").at("direction").at("description") ==
        "Cardinal direction: north, south, east, or west");

  const auto eat =
      nlohmann::json::parse(scry::reflection::input_schema_v<EatArguments>);
  CHECK(eat.at("type") == "object");
  CHECK(eat.at("properties").empty());
  CHECK(eat.at("required").empty());
  CHECK(eat.at("additionalProperties") == false);
}

TEST_CASE("Move returns a fixed typed result for success and wall failure") {
  World world{37};
  WorldTools tools{world};
  std::size_t turn = 1;

  tools.begin_turn(turn++);
  const auto east = tools.move({.direction = Direction::east});
  CHECK(east.response.ok);
  CHECK_FALSE(east.response.error);
  CHECK_FALSE(east.response.error_code);
  CHECK(east.response.position == (Position{.x = 6, .y = 5}));
  CHECK(east.response.action_executed);

  for (int y = world.position().y; y < World::height - 1; ++y) {
    tools.begin_turn(turn++);
    const auto moved = tools.move({.direction = Direction::north});
    REQUIRE(moved.response.ok);
  }
  const auto before_wall = world.position();
  tools.begin_turn(turn);
  const auto wall = tools.move({.direction = Direction::north});
  CHECK_FALSE(wall.response.ok);
  REQUIRE(wall.response.reason);
  CHECK(*wall.response.reason == pigpen::world::MoveFailure::wall);
  CHECK(wall.response.position == before_wall);
  CHECK(wall.response.action_executed);
  CHECK(wall.before == wall.after);
}

TEST_CASE("World-tool turn lifecycle is explicit and monotonic") {
  World world{2};
  WorldTools tools{world};

  CHECK_THROWS_AS(tools.eat({}), std::logic_error);
  tools.begin_turn(2);
  REQUIRE_NOTHROW(
      static_cast<void>(tools.look({.direction = Direction::north})));
  CHECK_THROWS_AS(tools.begin_turn(1), std::logic_error);
}

TEST_CASE("Per-turn budget returns a flat reflected failure without acting") {
  World world{37};
  WorldTools tools{world};
  const auto initial = world.position();
  tools.begin_turn(11);

  for (std::size_t used = 1;
       used <= pigpen::agent::max_world_tool_calls_per_turn; ++used) {
    const auto looked = tools.look({.direction = Direction::north});
    REQUIRE(looked.response.ok);
    CHECK_FALSE(looked.response.error);
    CHECK_FALSE(looked.response.error_code);
    CHECK(looked.response.turn_tool_budget.used == used);
    CHECK(looked.response.turn_tool_budget.remaining ==
          pigpen::agent::max_world_tool_calls_per_turn - used);
  }

  const auto rejected = tools.move({.direction = Direction::east});
  CHECK_FALSE(rejected.response.ok);
  CHECK_FALSE(rejected.response.action_executed);
  REQUIRE(rejected.response.error_code);
  CHECK(*rejected.response.error_code ==
        ToolFailureCode::tool_budget_exhausted);
  REQUIRE(rejected.response.error);
  CHECK(*rejected.response.error ==
        "No action was executed because this turn's world-tool call budget "
        "is exhausted.");
  CHECK(rejected.response.position == initial);
  CHECK(rejected.response.turn_tool_budget.used ==
        pigpen::agent::max_world_tool_calls_per_turn);
  CHECK(rejected.response.turn_tool_budget.remaining == 0);
  CHECK(world.position() == initial);

  tools.begin_turn(12);
  const auto next_turn = tools.move({.direction = Direction::east});
  REQUIRE(next_turn.response.ok);
  CHECK(next_turn.response.turn_tool_budget.used == 1);
  CHECK(next_turn.response.action_executed);
}

TEST_CASE("Look returns a typed complete ray and supports opaque items") {
  constexpr std::uint64_t seed = 73;
  World visible_world{seed};
  const auto placement = visible_world.items().front();
  const auto viewpoint = viewpoint_for(placement.position);
  move_to(visible_world, viewpoint.position);
  WorldTools visible_tools{visible_world};
  visible_tools.begin_turn(4);

  const auto visible = visible_tools.look({.direction = viewpoint.direction});
  REQUIRE(visible.response.ok);
  REQUIRE_FALSE(visible.response.cells.empty());
  REQUIRE(visible.response.cells.front().item);
  CHECK(*visible.response.cells.front().item ==
        pigpen::world::item_name(placement.item));
  CHECK(visible.response.wall_at_distance ==
        static_cast<int>(visible.response.cells.size()) + 1);

  World opaque_world{seed};
  move_to(opaque_world, viewpoint.position);
  Config config;
  config.opaque_look = true;
  WorldTools opaque_tools{opaque_world, config};
  opaque_tools.begin_turn(4);
  const auto opaque = opaque_tools.look({.direction = viewpoint.direction});
  REQUIRE(opaque.response.ok);
  REQUIRE(opaque.response.cells.front().item);
  CHECK(*opaque.response.cells.front().item == "something");
}

TEST_CASE(
    "Eat returns typed truth while reward visibility stays configurable") {
  constexpr std::uint64_t seed = 808;
  World feedback_world{seed};
  const auto placement = feedback_world.items().front();
  move_to(feedback_world, placement.position);
  WorldTools feedback_tools{feedback_world};
  feedback_tools.begin_turn(5);

  const auto revealed = feedback_tools.eat({});
  REQUIRE(revealed.response.ok);
  CHECK(revealed.response.ate == placement.item);
  CHECK(revealed.response.reward == pigpen::world::item_reward(placement.item));
  CHECK(revealed.response.score == pigpen::world::item_reward(placement.item));
  CHECK(revealed.eaten == placement.item);

  World hidden_world{seed};
  move_to(hidden_world, placement.position);
  Config config;
  config.reward_feedback = false;
  WorldTools hidden_tools{hidden_world, config};
  hidden_tools.begin_turn(5);
  const auto hidden = hidden_tools.eat({});
  REQUIRE(hidden.response.ok);
  CHECK_FALSE(hidden.response.reward);
  CHECK_FALSE(hidden.response.score);
  CHECK(hidden.eaten == placement.item);
  CHECK(hidden_world.score() == pigpen::world::item_reward(placement.item));
}
