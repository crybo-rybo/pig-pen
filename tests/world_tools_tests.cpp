/// @file world_tools_tests.cpp
/// @brief Covers the reflected tool boundary: compile-time input schemas,
/// typed world results and the
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
#include <string>

namespace {

using pigpen::agent::Config;
using pigpen::agent::DirectionArguments;
using pigpen::agent::EatArguments;
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

  const auto east = tools.move({.direction = Direction::east});
  CHECK(east.response.ok);
  CHECK(east.response.position == (Position{.x = 6, .y = 5}));

  for (int y = world.position().y; y < World::height - 1; ++y) {
    const auto moved = tools.move({.direction = Direction::north});
    REQUIRE(moved.response.ok);
  }
  const auto before_wall = world.position();
  const auto wall = tools.move({.direction = Direction::north});
  CHECK_FALSE(wall.response.ok);
  REQUIRE(wall.response.reason);
  CHECK(*wall.response.reason == pigpen::world::MoveFailure::wall);
  CHECK(wall.response.position == before_wall);
  CHECK(wall.before == wall.after);
}

TEST_CASE("Look returns a typed complete ray and supports opaque items") {
  constexpr std::uint64_t seed = 73;
  World visible_world{seed};
  const auto placement = visible_world.items().front();
  const auto viewpoint = viewpoint_for(placement.position);
  move_to(visible_world, viewpoint.position);
  WorldTools visible_tools{visible_world};

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
  const auto hidden = hidden_tools.eat({});
  REQUIRE(hidden.response.ok);
  CHECK_FALSE(hidden.response.reward);
  CHECK_FALSE(hidden.response.score);
  CHECK(hidden.eaten == placement.item);
  CHECK(hidden_world.score() == pigpen::world::item_reward(placement.item));
}

TEST_CASE(
    "Scry publicly encodes typed arguments and responses for observability") {
  World world{9};
  WorldTools tools{world};
  const auto execution = tools.move({.direction = Direction::east});
  const auto arguments = scry::reflection::encode(
      DirectionArguments{.direction = Direction::east});
  const auto response = scry::reflection::encode(execution.response);

  REQUIRE(arguments.has_value());
  REQUIRE(response.has_value());
  CHECK(arguments->text == R"({"direction":"east"})");
  const auto response_json = nlohmann::json::parse(response->text);
  CHECK(response_json.at("ok") == true);
  CHECK(response_json.at("position") == nlohmann::json{{"x", 6}, {"y", 5}});
  CHECK(response_json.at("reason").is_null());
  CHECK_FALSE(response_json.contains("result"));
}
