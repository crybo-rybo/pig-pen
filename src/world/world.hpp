#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::world {

struct Position {
  int x{};
  int y{};

  friend bool operator==(const Position &, const Position &) = default;
};

enum class Direction : std::uint8_t {
  north,
  south,
  east,
  west,
};

enum class ItemType : std::uint8_t {
  berry,
  apple,
  truffle,
  toadstool,
};

[[nodiscard]] constexpr std::string_view item_name(ItemType item) noexcept;
[[nodiscard]] constexpr int item_reward(ItemType item) noexcept;

enum class MoveFailure : std::uint8_t {
  wall,
};

enum class EatFailure : std::uint8_t {
  nothing_here,
};

[[nodiscard]] constexpr std::string_view
move_failure_name(MoveFailure failure) noexcept;
[[nodiscard]] constexpr std::string_view
eat_failure_name(EatFailure failure) noexcept;

struct MoveResult {
  bool ok{};
  Position position{};
  std::optional<ItemType> item_here{};
  std::optional<MoveFailure> failure{};

  friend bool operator==(const MoveResult &, const MoveResult &) = default;
};

struct LookCell {
  int distance{};
  Position position{};
  std::optional<ItemType> item{};

  friend bool operator==(const LookCell &, const LookCell &) = default;
};

struct LookResult {
  Direction direction{};
  std::vector<LookCell> cells{};
  int wall_at_distance{};

  friend bool operator==(const LookResult &, const LookResult &) = default;
};

struct EatResult {
  bool ok{};
  std::optional<ItemType> ate{};
  int reward{};
  int score{};
  std::optional<EatFailure> failure{};

  friend bool operator==(const EatResult &, const EatResult &) = default;
};

struct ItemPlacement {
  Position position{};
  ItemType item{};

  friend bool operator==(const ItemPlacement &,
                         const ItemPlacement &) = default;
};

class World final {
public:
  static constexpr int width = 10;
  static constexpr int height = 10;
  static constexpr std::size_t cell_count =
      static_cast<std::size_t>(width * height);
  static constexpr Position spawn{.x = 5, .y = 5};

  static constexpr std::size_t default_berry_count = 6;
  static constexpr std::size_t default_apple_count = 3;
  static constexpr std::size_t default_truffle_count = 1;
  static constexpr std::size_t default_toadstool_count = 3;
  static constexpr std::size_t default_item_count =
      default_berry_count + default_apple_count + default_truffle_count +
      default_toadstool_count;

  explicit World(std::uint64_t seed = 0);

  [[nodiscard]] static constexpr bool in_bounds(Position position) noexcept;

  [[nodiscard]] std::uint64_t seed() const noexcept;
  [[nodiscard]] Position position() const noexcept;
  [[nodiscard]] int score() const noexcept;
  [[nodiscard]] std::optional<ItemType>
  item_at(Position position) const noexcept;
  [[nodiscard]] bool is_observed(Position position) const noexcept;
  [[nodiscard]] std::vector<ItemPlacement> items() const;
  [[nodiscard]] std::vector<Position> observed_positions() const;
  [[nodiscard]] std::size_t remaining_count(ItemType item) const noexcept;
  [[nodiscard]] std::size_t eaten_count(ItemType item) const noexcept;

  [[nodiscard]] MoveResult move(Direction direction);
  [[nodiscard]] LookResult look(Direction direction);
  [[nodiscard]] EatResult eat();

  [[nodiscard]] bool all_positive_items_eaten() const noexcept;

  // A canonical, row-major representation of all simulation state. It is meant
  // for deterministic tests, diagnostics, and replay comparisons rather than
  // persistence.
  [[nodiscard]] std::string dump() const;

private:
  [[nodiscard]] static constexpr std::size_t index(Position position) noexcept;
  [[nodiscard]] static constexpr std::size_t item_index(ItemType item) noexcept;

  std::uint64_t seed_{};
  Position position_{spawn};
  int score_{};
  std::array<std::optional<ItemType>, cell_count> items_{};
  std::bitset<cell_count> observed_{};
  std::array<std::size_t, 4> eaten_counts_{};
};

constexpr std::string_view item_name(const ItemType item) noexcept {
  switch (item) {
  case ItemType::berry:
    return "berry";
  case ItemType::apple:
    return "apple";
  case ItemType::truffle:
    return "truffle";
  case ItemType::toadstool:
    return "toadstool";
  }
  return "unknown";
}

constexpr int item_reward(const ItemType item) noexcept {
  switch (item) {
  case ItemType::berry:
    return 1;
  case ItemType::apple:
    return 3;
  case ItemType::truffle:
    return 10;
  case ItemType::toadstool:
    return -5;
  }
  return 0;
}

constexpr std::string_view
move_failure_name(const MoveFailure failure) noexcept {
  switch (failure) {
  case MoveFailure::wall:
    return "wall";
  }
  return "unknown";
}

constexpr std::string_view eat_failure_name(const EatFailure failure) noexcept {
  switch (failure) {
  case EatFailure::nothing_here:
    return "nothing_here";
  }
  return "unknown";
}

constexpr bool World::in_bounds(const Position position) noexcept {
  return position.x >= 0 && position.x < width && position.y >= 0 &&
         position.y < height;
}

constexpr std::size_t World::index(const Position position) noexcept {
  return static_cast<std::size_t>(position.y * width + position.x);
}

constexpr std::size_t World::item_index(const ItemType item) noexcept {
  return static_cast<std::size_t>(item);
}

} // namespace pigpen::world
