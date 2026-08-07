#include "agent/tool_executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using pigpen::agent::Config;
using pigpen::agent::EventFeed;
using pigpen::agent::ToolErrorCode;
using pigpen::agent::ToolExecutor;
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
  std::string direction_text{};
};

[[nodiscard]] ItemViewpoint viewpoint_for(const Position item) {
  if (item.x > 0) {
    return {
        .position = {.x = item.x - 1, .y = item.y},
        .direction = Direction::east,
        .direction_text = "east",
    };
  }
  return {
      .position = {.x = item.x + 1, .y = item.y},
      .direction = Direction::west,
      .direction_text = "west",
  };
}

} // namespace

TEST_CASE("Tool schemas express the exact public argument contracts") {
  const auto &move = ToolExecutor::move_schema();
  CHECK(move.at("type") == "object");
  CHECK(move.at("required") == nlohmann::json::array({"direction"}));
  CHECK(move.at("additionalProperties") == false);
  CHECK(move.at("properties").size() == 1);
  CHECK(move.at("properties").at("direction").at("type") == "string");
  CHECK(move.at("properties").at("direction").at("enum") ==
        nlohmann::json::array({"north", "south", "east", "west"}));
  CHECK(ToolExecutor::look_schema() == move);

  const auto &eat = ToolExecutor::eat_schema();
  CHECK(eat == nlohmann::json{
                   {"type", "object"},
                   {"properties", nlohmann::json::object()},
                   {"additionalProperties", false},
               });
}

TEST_CASE(
    "Directional tools reject schema violations without world side effects") {
  World world{12};
  EventFeed events;
  ToolExecutor executor{world, events};
  const auto initial_dump = world.dump();
  const std::array invalid_arguments{
      nlohmann::json(nullptr),
      nlohmann::json::array(),
      nlohmann::json::object(),
      nlohmann::json{{"direction", "north"}, {"extra", true}},
      nlohmann::json{{"direction", 1}},
      nlohmann::json{{"direction", "up"}},
  };

  for (const auto &arguments : invalid_arguments) {
    const auto move = executor.execute(1, "move", arguments);
    REQUIRE_FALSE(move);
    CHECK(move.error().code == ToolErrorCode::invalid_arguments);

    const auto look = executor.execute(1, "look", arguments);
    REQUIRE_FALSE(look);
    CHECK(look.error().code == ToolErrorCode::invalid_arguments);
  }

  CHECK(events.size() == invalid_arguments.size() * 2);
  CHECK(executor.next_tick() == events.size() + 1);
  CHECK(events.front().result.at("error_code") == "invalid_arguments");
  CHECK(world.dump() == initial_dump);
}

TEST_CASE("Eat accepts only an empty object") {
  World world{91};
  EventFeed events;
  ToolExecutor executor{world, events};

  for (const auto &arguments :
       std::array{nlohmann::json(nullptr), nlohmann::json::array(),
                  nlohmann::json{{"anything", true}}}) {
    const auto result = executor.execute(2, "eat", arguments);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ToolErrorCode::invalid_arguments);
  }

  CHECK(events.size() == 3);
  CHECK(events.back().result.at("error_code") == "invalid_arguments");
  const auto valid = executor.execute(2, "eat", nlohmann::json::object());
  REQUIRE(valid);
  CHECK(*valid == nlohmann::json{{"ok", false}, {"reason", "nothing_here"}});
  REQUIRE(events.size() == 4);
}

TEST_CASE("Serialized and unknown tool failures are typed and observable") {
  World world{4};
  EventFeed events;
  ToolExecutor executor{world, events};

  const auto malformed =
      executor.execute_serialized(1, "move", R"({"direction":)");
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().code == ToolErrorCode::malformed_json);

  const auto unknown =
      executor.execute(1, "teleport", nlohmann::json::object());
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().code == ToolErrorCode::unknown_tool);
  REQUIRE(events.size() == 2);
  CHECK(events[0].result.at("error_code") == "malformed_json");
  CHECK(events[1].result.at("error_code") == "unknown_tool");
}

TEST_CASE("Move returns structured success and wall failure results") {
  World world{37};
  EventFeed events;
  ToolExecutor executor{world, events};

  const auto east = executor.execute(3, "move", {{"direction", "east"}});
  REQUIRE(east);
  CHECK(east->at("ok") == true);
  CHECK(east->at("position") == nlohmann::json{{"x", 6}, {"y", 5}});
  CHECK(east->contains("item_here"));

  for (int y = world.position().y; y < World::height - 1; ++y) {
    REQUIRE(executor.execute(3, "move", {{"direction", "north"}}));
  }
  const auto before_wall = world.position();
  const auto wall = executor.execute(3, "move", {{"direction", "north"}});
  REQUIRE(wall);
  CHECK(*wall ==
        nlohmann::json{
            {"ok", false},
            {"reason", "wall"},
            {"position", {{"x", before_wall.x}, {"y", before_wall.y}}}});
  CHECK(world.position() == before_wall);
}

TEST_CASE("Look reports a complete ray and can hide item identity") {
  constexpr std::uint64_t seed = 73;
  World transparent_world{seed};
  const auto placement = transparent_world.items().front();
  const auto viewpoint = viewpoint_for(placement.position);
  move_to(transparent_world, viewpoint.position);
  EventFeed transparent_events;
  ToolExecutor transparent{transparent_world, transparent_events};

  const auto visible =
      transparent.execute(4, "look", {{"direction", viewpoint.direction_text}});
  REQUIRE(visible);
  REQUIRE_FALSE(visible->at("cells").empty());
  CHECK(visible->at("direction") == viewpoint.direction_text);
  CHECK(visible->at("cells").front().at("distance") == 1);
  CHECK(visible->at("cells").front().at("item") ==
        pigpen::world::item_name(placement.item));
  CHECK(visible->at("wall_at_distance") ==
        static_cast<int>(visible->at("cells").size()) + 1);

  World opaque_world{seed};
  move_to(opaque_world, viewpoint.position);
  EventFeed opaque_events;
  Config opaque_config;
  opaque_config.opaque_look = true;
  ToolExecutor opaque{opaque_world, opaque_events, opaque_config};
  const auto hidden =
      opaque.execute(4, "look", {{"direction", viewpoint.direction_text}});
  REQUIRE(hidden);
  CHECK(hidden->at("cells").front().at("item") == "something");
  CHECK(hidden->at("cells").front().at("item") !=
        pigpen::world::item_name(placement.item));
}

TEST_CASE("Eat feedback toggle changes only model-visible numeric fields") {
  constexpr std::uint64_t seed = 808;
  World feedback_world{seed};
  const auto placement = feedback_world.items().front();
  move_to(feedback_world, placement.position);
  EventFeed feedback_events;
  ToolExecutor feedback{feedback_world, feedback_events};

  const auto revealed = feedback.execute(5, "eat", nlohmann::json::object());
  REQUIRE(revealed);
  CHECK(revealed->at("ok") == true);
  CHECK(revealed->at("ate") == pigpen::world::item_name(placement.item));
  CHECK(revealed->at("reward") == pigpen::world::item_reward(placement.item));
  CHECK(revealed->at("score") == pigpen::world::item_reward(placement.item));

  World hidden_world{seed};
  move_to(hidden_world, placement.position);
  EventFeed hidden_events;
  Config hidden_config;
  hidden_config.reward_feedback = false;
  ToolExecutor hidden{hidden_world, hidden_events, hidden_config};
  const auto withheld = hidden.execute(5, "eat", nlohmann::json::object());
  REQUIRE(withheld);
  CHECK(withheld->at("ok") == true);
  CHECK(withheld->at("ate") == pigpen::world::item_name(placement.item));
  CHECK_FALSE(withheld->contains("reward"));
  CHECK_FALSE(withheld->contains("score"));
  CHECK(hidden_world.score() == pigpen::world::item_reward(placement.item));
}

TEST_CASE("Every schema-valid call appends one monotonic complete event") {
  World world{21};
  EventFeed events;
  ToolExecutor executor{world, events};
  const auto initial = world.position();

  REQUIRE(executor.execute(7, "look", {{"direction", "west"}}));
  REQUIRE(executor.execute(8, "move", {{"direction", "south"}}));
  REQUIRE(executor.execute(8, "eat", nlohmann::json::object()));

  REQUIRE(events.size() == 3);
  CHECK(events[0].tick == 1);
  CHECK(events[1].tick == 2);
  CHECK(events[2].tick == 3);
  CHECK(executor.next_tick() == 4);
  CHECK(events[0].turn == 7);
  CHECK(events[0].tool == "look");
  CHECK(events[0].arguments == nlohmann::json{{"direction", "west"}});
  CHECK(events[0].result.at("direction") == "west");
  CHECK(events[0].before == initial);
  CHECK(events[0].after == initial);
  REQUIRE(events[0].direction);
  CHECK(*events[0].direction == Direction::west);

  CHECK(events[1].before == initial);
  CHECK(events[1].after == (Position{.x = 5, .y = 4}));
  REQUIRE(events[1].direction);
  CHECK(*events[1].direction == Direction::south);
  CHECK_FALSE(events[2].direction);
  CHECK(events[2].before == events[2].after);
}
