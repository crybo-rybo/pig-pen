#include "agent/reflected_json.hpp"
#include "agent/tool_contract.hpp"
#include "agent/world_tools.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <scry/reflection.hpp>
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

struct ItemViewpoint {
  Position position{};
  Direction direction{};
};

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
  REQUIRE(east.response.result);
  CHECK_FALSE(east.response.error);
  CHECK(east.response.result->ok);
  CHECK(east.response.result->position == (Position{.x = 6, .y = 5}));
  CHECK(east.action_executed);

  for (int y = world.position().y; y < World::height - 1; ++y) {
    tools.begin_turn(turn++);
    const auto moved = tools.move({.direction = Direction::north});
    REQUIRE(moved.response.result);
    REQUIRE(moved.response.result->ok);
  }
  const auto before_wall = world.position();
  tools.begin_turn(turn);
  const auto wall = tools.move({.direction = Direction::north});
  REQUIRE(wall.response.result);
  CHECK_FALSE(wall.response.result->ok);
  REQUIRE(wall.response.result->reason);
  CHECK(*wall.response.result->reason == pigpen::world::MoveFailure::wall);
  CHECK(wall.response.result->position == before_wall);
  CHECK(wall.action_executed);
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

TEST_CASE("Per-turn budget returns a reflected error envelope without acting") {
  World world{37};
  WorldTools tools{world};
  const auto initial = world.position();
  tools.begin_turn(11);

  for (std::size_t used = 1;
       used <= pigpen::agent::max_world_tool_calls_per_turn; ++used) {
    const auto looked = tools.look({.direction = Direction::north});
    REQUIRE(looked.response.result);
    CHECK_FALSE(looked.response.error);
    CHECK(looked.response.turn_tool_budget.used == used);
    CHECK(looked.response.turn_tool_budget.remaining ==
          pigpen::agent::max_world_tool_calls_per_turn - used);
  }

  const auto rejected = tools.move({.direction = Direction::east});
  CHECK_FALSE(rejected.response.result);
  REQUIRE(rejected.response.error);
  CHECK(rejected.response.error->code ==
        ToolFailureCode::tool_budget_exhausted);
  CHECK(rejected.response.turn_tool_budget.used ==
        pigpen::agent::max_world_tool_calls_per_turn);
  CHECK(rejected.response.turn_tool_budget.remaining == 0);
  CHECK_FALSE(rejected.action_executed);
  CHECK(world.position() == initial);

  tools.begin_turn(12);
  const auto next_turn = tools.move({.direction = Direction::east});
  REQUIRE(next_turn.response.result);
  CHECK(next_turn.response.result->ok);
  CHECK(next_turn.response.turn_tool_budget.used == 1);
  CHECK(next_turn.action_executed);
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
  REQUIRE(visible.response.result);
  REQUIRE_FALSE(visible.response.result->cells.empty());
  REQUIRE(visible.response.result->cells.front().item);
  CHECK(*visible.response.result->cells.front().item ==
        pigpen::world::item_name(placement.item));
  CHECK(visible.response.result->wall_at_distance ==
        static_cast<int>(visible.response.result->cells.size()) + 1);

  World opaque_world{seed};
  move_to(opaque_world, viewpoint.position);
  Config config;
  config.opaque_look = true;
  WorldTools opaque_tools{opaque_world, config};
  opaque_tools.begin_turn(4);
  const auto opaque = opaque_tools.look({.direction = viewpoint.direction});
  REQUIRE(opaque.response.result);
  REQUIRE(opaque.response.result->cells.front().item);
  CHECK(*opaque.response.result->cells.front().item == "something");
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
  REQUIRE(revealed.response.result);
  CHECK(revealed.response.result->ok);
  CHECK(revealed.response.result->ate == placement.item);
  CHECK(revealed.response.result->reward ==
        pigpen::world::item_reward(placement.item));
  CHECK(revealed.response.result->score ==
        pigpen::world::item_reward(placement.item));
  CHECK(revealed.eaten == placement.item);

  World hidden_world{seed};
  move_to(hidden_world, placement.position);
  Config config;
  config.reward_feedback = false;
  WorldTools hidden_tools{hidden_world, config};
  hidden_tools.begin_turn(5);
  const auto hidden = hidden_tools.eat({});
  REQUIRE(hidden.response.result);
  CHECK(hidden.response.result->ok);
  CHECK_FALSE(hidden.response.result->reward);
  CHECK_FALSE(hidden.response.result->score);
  CHECK(hidden.eaten == placement.item);
  CHECK(hidden_world.score() == pigpen::world::item_reward(placement.item));
}

TEST_CASE("Reflection projects the typed result envelope for observability") {
  World world{9};
  WorldTools tools{world};
  tools.begin_turn(1);
  const auto execution = tools.move({.direction = Direction::east});
  const auto arguments = pigpen::agent::reflected_json(
      DirectionArguments{.direction = Direction::east});
  const auto response = pigpen::agent::reflected_json(execution.response);

  CHECK(arguments == nlohmann::json{{"direction", "east"}});
  CHECK(response.at("error").is_null());
  CHECK(response.at("result").at("ok") == true);
  CHECK(response.at("result").at("position") ==
        nlohmann::json{{"x", 6}, {"y", 5}});
  CHECK(response.at("result").at("reason").is_null());
  CHECK(response.at("turn_tool_budget").at("used") == 1);
}
