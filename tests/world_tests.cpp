#include "world/world.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace {

using pigpen::world::Direction;
using pigpen::world::EatFailure;
using pigpen::world::ItemPlacement;
using pigpen::world::ItemType;
using pigpen::world::MoveFailure;
using pigpen::world::Position;
using pigpen::world::World;

[[nodiscard]] int manhattan_distance(const Position lhs, const Position rhs) {
  const auto x_distance = lhs.x > rhs.x ? lhs.x - rhs.x : rhs.x - lhs.x;
  const auto y_distance = lhs.y > rhs.y ? lhs.y - rhs.y : rhs.y - lhs.y;
  return x_distance + y_distance;
}

[[nodiscard]] std::size_t
count_item(const std::vector<ItemPlacement> &placements, const ItemType item) {
  return static_cast<std::size_t>(
      std::count_if(placements.begin(), placements.end(),
                    [item](const auto &value) { return value.item == item; }));
}

[[nodiscard]] Position find_item(const std::vector<ItemPlacement> &placements,
                                 const ItemType item) {
  const auto found =
      std::find_if(placements.begin(), placements.end(),
                   [item](const auto &value) { return value.item == item; });
  REQUIRE(found != placements.end());
  return found->position;
}

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
  REQUIRE(world.position() == destination);
}

} // namespace

TEST_CASE("world constants and names match the pen contract", "[world]") {
  CHECK(World::width == 10);
  CHECK(World::height == 10);
  CHECK(World::spawn == Position{5, 5});
  CHECK(World::default_item_count == 13);

  CHECK(pigpen::world::item_name(ItemType::berry) == "berry");
  CHECK(pigpen::world::item_name(ItemType::apple) == "apple");
  CHECK(pigpen::world::item_name(ItemType::truffle) == "truffle");
  CHECK(pigpen::world::item_name(ItemType::toadstool) == "toadstool");
  CHECK(pigpen::world::item_reward(ItemType::berry) == 1);
  CHECK(pigpen::world::item_reward(ItemType::apple) == 3);
  CHECK(pigpen::world::item_reward(ItemType::truffle) == 10);
  CHECK(pigpen::world::item_reward(ItemType::toadstool) == -5);
  CHECK(pigpen::world::move_failure_name(MoveFailure::wall) == "wall");
  CHECK(pigpen::world::eat_failure_name(EatFailure::nothing_here) ==
        "nothing_here");
}

TEST_CASE("default placement has the specified unique items",
          "[world][placement]") {
  for (std::uint64_t seed = 0; seed < 128; ++seed) {
    const World world{seed};
    const auto placements = world.items();
    CAPTURE(seed);

    REQUIRE(placements.size() == World::default_item_count);
    CHECK(count_item(placements, ItemType::berry) ==
          World::default_berry_count);
    CHECK(count_item(placements, ItemType::apple) ==
          World::default_apple_count);
    CHECK(count_item(placements, ItemType::truffle) ==
          World::default_truffle_count);
    CHECK(count_item(placements, ItemType::toadstool) ==
          World::default_toadstool_count);

    std::set<std::pair<int, int>> occupied;
    for (const auto &placement : placements) {
      CHECK(World::in_bounds(placement.position));
      CHECK(placement.position != World::spawn);
      CHECK(
          occupied.emplace(placement.position.x, placement.position.y).second);
      if (placement.item == ItemType::truffle) {
        CHECK(manhattan_distance(placement.position, World::spawn) >= 4);
      }
    }
  }
}

TEST_CASE("the spawn is empty and is the only initially observed cell",
          "[world][observation]") {
  const World world{42};

  CHECK(world.seed() == 42);
  CHECK(world.position() == World::spawn);
  CHECK(world.score() == 0);
  CHECK_FALSE(world.item_at(World::spawn).has_value());
  CHECK(world.is_observed(World::spawn));
  CHECK(world.observed_positions() == std::vector<Position>{World::spawn});

  CHECK_FALSE(World::in_bounds({.x = -1, .y = 0}));
  CHECK_FALSE(World::in_bounds({.x = 0, .y = -1}));
  CHECK_FALSE(World::in_bounds({.x = World::width, .y = 0}));
  CHECK_FALSE(World::in_bounds({.x = 0, .y = World::height}));
  CHECK_FALSE(world.item_at({.x = -1, .y = 0}).has_value());
  CHECK_FALSE(world.is_observed({.x = World::width, .y = World::height}));
}

TEST_CASE("moving follows east-positive and north-positive coordinates",
          "[world][move]") {
  World world{7};

  const auto north = world.move(Direction::north);
  REQUIRE(north.ok);
  CHECK(north.position == Position{5, 6});
  CHECK(north.item_here == world.item_at(north.position));
  CHECK_FALSE(north.failure.has_value());
  CHECK(world.is_observed(north.position));

  const auto east = world.move(Direction::east);
  REQUIRE(east.ok);
  CHECK(east.position == Position{6, 6});

  const auto south = world.move(Direction::south);
  REQUIRE(south.ok);
  CHECK(south.position == Position{6, 5});

  const auto west = world.move(Direction::west);
  REQUIRE(west.ok);
  CHECK(west.position == World::spawn);
}

TEST_CASE("moving into each wall is a typed failure without mutation",
          "[world][move]") {
  const auto verify_wall = [](const Direction direction,
                              const int successful_moves, const Position edge) {
    World world{19};
    for (int step_number = 0; step_number < successful_moves; ++step_number) {
      REQUIRE(world.move(direction).ok);
    }
    REQUIRE(world.position() == edge);
    const auto before_observed = world.observed_positions();

    const auto blocked = world.move(direction);

    CHECK_FALSE(blocked.ok);
    CHECK(blocked.position == edge);
    CHECK_FALSE(blocked.item_here.has_value());
    REQUIRE(blocked.failure.has_value());
    CHECK(*blocked.failure == MoveFailure::wall);
    CHECK(world.position() == edge);
    CHECK(world.observed_positions() == before_observed);
  };

  SECTION("north") { verify_wall(Direction::north, 4, {.x = 5, .y = 9}); }
  SECTION("south") { verify_wall(Direction::south, 5, {.x = 5, .y = 0}); }
  SECTION("east") { verify_wall(Direction::east, 4, {.x = 9, .y = 5}); }
  SECTION("west") { verify_wall(Direction::west, 5, {.x = 0, .y = 5}); }
}

TEST_CASE("look returns every cell in its ray and marks it observed",
          "[world][look]") {
  const struct RayCase {
    Direction direction;
    Position delta;
    int cell_count;
  } ray_cases[] = {
      {Direction::north, {.x = 0, .y = 1}, 4},
      {Direction::south, {.x = 0, .y = -1}, 5},
      {Direction::east, {.x = 1, .y = 0}, 4},
      {Direction::west, {.x = -1, .y = 0}, 5},
  };

  for (const auto &ray_case : ray_cases) {
    World world{23};
    const auto result = world.look(ray_case.direction);
    CAPTURE(static_cast<int>(ray_case.direction));

    CHECK(result.direction == ray_case.direction);
    REQUIRE(result.cells.size() ==
            static_cast<std::size_t>(ray_case.cell_count));
    CHECK(result.wall_at_distance == ray_case.cell_count + 1);
    for (int offset = 0; offset < ray_case.cell_count; ++offset) {
      const auto distance = offset + 1;
      const Position expected{
          .x = World::spawn.x + ray_case.delta.x * distance,
          .y = World::spawn.y + ray_case.delta.y * distance,
      };
      const auto &cell = result.cells[static_cast<std::size_t>(offset)];
      CHECK(cell.distance == distance);
      CHECK(cell.position == expected);
      CHECK(cell.item == world.item_at(expected));
      CHECK(world.is_observed(expected));
    }
    CHECK(world.observed_positions().size() == result.cells.size() + 1);
    CHECK(world.position() == World::spawn);
  }
}

TEST_CASE("eating reports empty cells without changing the score",
          "[world][eat]") {
  World world{31};

  const auto result = world.eat();

  CHECK_FALSE(result.ok);
  CHECK_FALSE(result.ate.has_value());
  CHECK(result.reward == 0);
  CHECK(result.score == 0);
  REQUIRE(result.failure.has_value());
  CHECK(*result.failure == EatFailure::nothing_here);
  CHECK(world.score() == 0);
}

TEST_CASE("eating consumes each item and applies its reward", "[world][eat]") {
  World world{37};
  const auto initial_items = world.items();
  int expected_score = 0;

  for (const auto item : std::array{ItemType::berry, ItemType::apple,
                                    ItemType::truffle, ItemType::toadstool}) {
    const auto target = find_item(initial_items, item);
    move_to(world, target);
    REQUIRE(world.item_at(target) == item);

    const auto result = world.eat();
    expected_score += pigpen::world::item_reward(item);

    REQUIRE(result.ok);
    REQUIRE(result.ate.has_value());
    CHECK(*result.ate == item);
    CHECK(result.reward == pigpen::world::item_reward(item));
    CHECK(result.score == expected_score);
    CHECK_FALSE(result.failure.has_value());
    CHECK_FALSE(world.item_at(target).has_value());
    CHECK(world.eaten_count(item) == 1);
    CHECK(world.remaining_count(item) == count_item(initial_items, item) - 1);

    const auto second_attempt = world.eat();
    CHECK_FALSE(second_attempt.ok);
    CHECK(second_attempt.score == expected_score);
  }
  CHECK(world.score() == expected_score);
}

TEST_CASE("positive-item exhaustion ignores remaining poison", "[world][eat]") {
  World world{41};
  const auto initial_items = world.items();
  CHECK_FALSE(world.all_positive_items_eaten());

  for (const auto &placement : initial_items) {
    if (pigpen::world::item_reward(placement.item) <= 0) {
      continue;
    }
    move_to(world, placement.position);
    REQUIRE(world.eat().ok);
  }

  CHECK(world.all_positive_items_eaten());
  CHECK(world.score() == 25);
  CHECK(world.remaining_count(ItemType::berry) == 0);
  CHECK(world.remaining_count(ItemType::apple) == 0);
  CHECK(world.remaining_count(ItemType::truffle) == 0);
  CHECK(world.remaining_count(ItemType::toadstool) ==
        World::default_toadstool_count);
  CHECK(world.eaten_count(ItemType::berry) == World::default_berry_count);
  CHECK(world.eaten_count(ItemType::apple) == World::default_apple_count);
  CHECK(world.eaten_count(ItemType::truffle) == World::default_truffle_count);
}

TEST_CASE("seed and action sequence completely determine the dump",
          "[world][determinism]") {
  World first{0xC0FFEE};
  World second{0xC0FFEE};
  const World different_seed{0xC0FFEF};

  REQUIRE(first.dump() == second.dump());
  CHECK(first.dump() != different_seed.dump());
  CHECK(first.items() == second.items());

  const auto exercise = [](World &world) {
    static_cast<void>(world.look(Direction::north));
    static_cast<void>(world.move(Direction::west));
    static_cast<void>(world.look(Direction::south));
    static_cast<void>(world.move(Direction::south));
    static_cast<void>(world.eat());
  };
  exercise(first);
  exercise(second);

  CHECK(first.dump() == second.dump());
  CHECK(first.position() == second.position());
  CHECK(first.items() == second.items());
  CHECK(first.observed_positions() == second.observed_positions());
}
