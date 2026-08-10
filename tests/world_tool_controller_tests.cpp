/// @file world_tool_controller_tests.cpp
/// @brief Covers the application service behind the reflected boundary: one
/// typed activity per decoded call, truthful outcomes and summaries, budget
/// rejections that never mutate the world, and typed responses returned
/// unchanged to the caller (scry).

#include "agent/world_tool_controller.hpp"

#include "agent/tool_activity.hpp"
#include "agent/tool_contract.hpp"
#include "agent/world_tools.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace {

using pigpen::agent::Config;
using pigpen::agent::ToolActivity;
using pigpen::agent::ToolKind;
using pigpen::agent::ToolOutcome;
using pigpen::agent::WorldToolController;
using pigpen::agent::WorldTools;
using pigpen::world::Direction;
using pigpen::world::Position;
using pigpen::world::World;

/// @brief Test sink capturing every published activity in order.
class RecordingSink final : public pigpen::agent::IToolActivitySink {
public:
  void publish(ToolActivity activity) override {
    activities.push_back(std::move(activity));
  }

  std::vector<ToolActivity> activities{};
};

/// @brief Controller plus everything it orchestrates, on a fixed seed.
struct ControllerFixture {
  explicit ControllerFixture(const std::uint64_t seed, Config config = {})
      : world{seed}, tools{world, std::move(config)},
        controller{tools, sink, [this] { return turn; }} {}

  World world;
  WorldTools tools;
  RecordingSink sink;
  std::size_t turn{1};
  WorldToolController controller;
};

/// @brief Walks the blob to @p destination one legal step at a time.
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

} // namespace

TEST_CASE("a decoded move publishes one truthful activity and returns the "
          "typed response") {
  ControllerFixture fixture{37};

  const auto response = fixture.controller.move({.direction = Direction::east});
  CHECK(response.ok);
  CHECK(response.action_executed);
  CHECK(response.position == (Position{.x = 6, .y = 5}));
  CHECK(fixture.world.position() == (Position{.x = 6, .y = 5}));

  REQUIRE(fixture.sink.activities.size() == 1);
  const auto &activity = fixture.sink.activities.front();
  CHECK(activity.turn == 1);
  CHECK(activity.kind == ToolKind::move);
  CHECK(activity.outcome == ToolOutcome::succeeded);
  CHECK(activity.before == (Position{.x = 5, .y = 5}));
  CHECK(activity.after == (Position{.x = 6, .y = 5}));
  REQUIRE(activity.direction);
  CHECK(*activity.direction == Direction::east);
  CHECK_FALSE(activity.eaten);
  CHECK(activity.score_after == 0);
  CHECK(activity.summary == "move east: (5, 5) -> (6, 5)");
}

TEST_CASE("a wall-blocked move is one executed-but-failed activity") {
  ControllerFixture fixture{37};
  move_to(fixture.world, {.x = 5, .y = World::height - 1});

  const auto response =
      fixture.controller.move({.direction = Direction::north});
  CHECK_FALSE(response.ok);
  CHECK(response.action_executed);

  REQUIRE(fixture.sink.activities.size() == 1);
  const auto &activity = fixture.sink.activities.front();
  CHECK(activity.outcome == ToolOutcome::blocked_by_wall);
  CHECK(activity.action_executed());
  CHECK_FALSE(activity.succeeded());
  CHECK(activity.before == activity.after);
  CHECK(activity.summary == "move north blocked by the wall at (5, 9)");
}

TEST_CASE("look publishes a truthful occupancy summary") {
  ControllerFixture fixture{73};

  const auto response =
      fixture.controller.look({.direction = Direction::north});
  REQUIRE(response.ok);

  REQUIRE(fixture.sink.activities.size() == 1);
  const auto &activity = fixture.sink.activities.front();
  CHECK(activity.kind == ToolKind::look);
  CHECK(activity.outcome == ToolOutcome::succeeded);
  CHECK(activity.before == activity.after);

  std::size_t occupied = 0;
  for (const auto &cell : response.cells) {
    occupied += cell.item ? 1U : 0U;
  }
  CHECK(activity.summary == "look north: " + std::to_string(occupied) +
                                " occupied of " +
                                std::to_string(response.cells.size()) +
                                " cells, wall at distance " +
                                std::to_string(response.wall_at_distance));
}

TEST_CASE("eating an item records the item and truthful reward and score") {
  ControllerFixture fixture{808};
  const auto placement = fixture.world.items().front();
  move_to(fixture.world, placement.position);

  const auto response = fixture.controller.eat({});
  REQUIRE(response.ok);
  CHECK(response.ate == placement.item);

  REQUIRE(fixture.sink.activities.size() == 1);
  const auto &activity = fixture.sink.activities.front();
  CHECK(activity.kind == ToolKind::eat);
  CHECK(activity.outcome == ToolOutcome::succeeded);
  REQUIRE(activity.eaten);
  CHECK(*activity.eaten == placement.item);
  CHECK(activity.score_after == pigpen::world::item_reward(placement.item));
  CHECK_FALSE(activity.direction);
}

TEST_CASE("the activity stays truthful when reward feedback is hidden from "
          "the model") {
  Config config;
  config.reward_feedback = false;
  ControllerFixture fixture{808, config};
  const auto placement = fixture.world.items().front();
  move_to(fixture.world, placement.position);

  const auto response = fixture.controller.eat({});
  REQUIRE(response.ok);
  // The model sees nothing; the journal and log still see the truth.
  CHECK_FALSE(response.reward);
  CHECK_FALSE(response.score);

  REQUIRE(fixture.sink.activities.size() == 1);
  const auto &activity = fixture.sink.activities.front();
  CHECK(activity.score_after == pigpen::world::item_reward(placement.item));
  REQUIRE(activity.eaten);
  CHECK(*activity.eaten == placement.item);
}

TEST_CASE("eating on an empty cell is an executed nothing_to_eat activity") {
  ControllerFixture fixture{37};

  auto empty = Position{.x = 5, .y = 5};
  // The spawn cell may hold an item on some seeds; step until it is empty.
  while (fixture.world.item_at(empty)) {
    empty.x -= 1;
    move_to(fixture.world, empty);
  }

  const auto response = fixture.controller.eat({});
  CHECK_FALSE(response.ok);
  CHECK(response.action_executed);

  REQUIRE(fixture.sink.activities.size() == 1);
  const auto &activity = fixture.sink.activities.front();
  CHECK(activity.outcome == ToolOutcome::nothing_to_eat);
  CHECK_FALSE(activity.eaten);
}

TEST_CASE("a budget-rejected call still publishes one activity with no world "
          "mutation") {
  ControllerFixture fixture{37};
  const auto initial = fixture.world.position();

  for (std::size_t used = 0;
       used < pigpen::agent::max_world_tool_calls_per_turn; ++used) {
    REQUIRE(fixture.controller.look({.direction = Direction::north}).ok);
  }

  const auto rejected = fixture.controller.move({.direction = Direction::east});
  CHECK_FALSE(rejected.ok);
  CHECK_FALSE(rejected.action_executed);
  CHECK(fixture.world.position() == initial);

  REQUIRE(fixture.sink.activities.size() ==
          pigpen::agent::max_world_tool_calls_per_turn + 1);
  const auto &activity = fixture.sink.activities.back();
  CHECK(activity.outcome == ToolOutcome::budget_exhausted);
  CHECK_FALSE(activity.action_executed());
  CHECK(activity.before == initial);
  CHECK(activity.after == initial);
  CHECK(activity.summary == "move east rejected: turn tool budget exhausted");

  // Advancing the turn reopens the budget through the controller.
  fixture.turn = 2;
  const auto next = fixture.controller.move({.direction = Direction::east});
  CHECK(next.ok);
  CHECK(next.turn_tool_budget.used == 1);
}

TEST_CASE("the controller resolves the turn at dispatch time") {
  ControllerFixture fixture{37};

  REQUIRE(fixture.controller.look({.direction = Direction::north}).ok);
  fixture.turn = 3;
  REQUIRE(fixture.controller.look({.direction = Direction::south}).ok);

  REQUIRE(fixture.sink.activities.size() == 2);
  CHECK(fixture.sink.activities[0].turn == 1);
  CHECK(fixture.sink.activities[1].turn == 3);
}
