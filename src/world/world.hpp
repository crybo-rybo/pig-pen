/// @file world.hpp
/// @brief The deterministic 10x10 pen simulation as a pure value type.
///
/// No JSON, no networking, no callbacks. Actions return typed results with
/// enumerated failures, and the same seed plus the same action sequence
/// always reproduces the same world.
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

/// @brief A grid coordinate; (0,0) is the south-west corner, x grows east
/// and y grows north.
struct Position {
  int x{};
  int y{};

  friend bool operator==(const Position &, const Position &) = default;
};

/// @brief Cardinal direction for movement and scanning.
/// @note Enumerator spelling is protocol-visible: scry uses the identifiers
/// as the JSON strings the model sends and receives.
enum class Direction : std::uint8_t {
  north,
  south,
  east,
  west,
};

/// @brief Canonical lowercase name of a direction, matching its reflected
/// protocol spelling.
[[nodiscard]] constexpr std::string_view
direction_name(Direction direction) noexcept;

/// @brief The four edible item kinds placed by the seed.
/// @note Enumerator spelling is protocol-visible through scry and the log.
enum class ItemType : std::uint8_t {
  berry,
  apple,
  truffle,
  toadstool,
};

/// @brief Canonical lowercase name of an item, as shown to the model.
[[nodiscard]] constexpr std::string_view item_name(ItemType item) noexcept;
/// @brief Score delta for eating an item (toadstools are negative).
[[nodiscard]] constexpr int item_reward(ItemType item) noexcept;

/// @brief Why a move did not change position.
enum class MoveFailure : std::uint8_t {
  wall,
};

/// @brief Why an eat consumed nothing.
enum class EatFailure : std::uint8_t {
  nothing_here,
};

/// @brief Canonical name of a move failure, as reported to the model.
[[nodiscard]] constexpr std::string_view
move_failure_name(MoveFailure failure) noexcept;
/// @brief Canonical name of an eat failure, as reported to the model.
[[nodiscard]] constexpr std::string_view
eat_failure_name(EatFailure failure) noexcept;

/// @brief Outcome of a single-cell move.
/// @note Moving onto an item never collects it; @c item_here reports what is
/// underfoot so the caller (or the model) can decide to eat.
struct MoveResult {
  bool ok{};
  Position position{};
  std::optional<ItemType> item_here{};
  std::optional<MoveFailure> failure{};

  friend bool operator==(const MoveResult &, const MoveResult &) = default;
};

/// @brief One cell revealed by a look ray, at @c distance from the blob.
struct LookCell {
  int distance{};
  Position position{};
  std::optional<ItemType> item{};

  friend bool operator==(const LookCell &, const LookCell &) = default;
};

/// @brief Every cell from the blob to the wall in one direction.
/// @note @c wall_at_distance is one past the last in-bounds cell, so a blob
/// directly against the wall reports an empty @c cells and distance 1.
struct LookResult {
  Direction direction{};
  std::vector<LookCell> cells{};
  int wall_at_distance{};

  friend bool operator==(const LookResult &, const LookResult &) = default;
};

/// @brief Outcome of eating whatever is underfoot; @c score is cumulative.
struct EatResult {
  bool ok{};
  std::optional<ItemType> ate{};
  int reward{};
  int score{};
  std::optional<EatFailure> failure{};

  friend bool operator==(const EatResult &, const EatResult &) = default;
};

/// @brief An item and where it sits, for omniscient observers (UI, logs).
struct ItemPlacement {
  Position position{};
  ItemType item{};

  friend bool operator==(const ItemPlacement &,
                         const ItemPlacement &) = default;
};

/// @brief The whole simulation: seed, blob position, score, item grid,
/// observed bitset, and per-item eaten counts.
///
/// Construction from a seed fully determines item placement. All mutation
/// goes through move(), look(), and eat(), so replaying a recorded action
/// sequence reproduces the exact same state.
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

  /// @brief Seeds item placement and marks the spawn cell observed.
  ///
  /// The truffle is placed first, at least 4 Manhattan steps from spawn so
  /// the jackpot is never trivially adjacent; the remaining items draw from
  /// the leftover cells.
  explicit World(std::uint64_t seed = 0);

  /// @brief Whether a position lies inside the walled grid.
  [[nodiscard]] static constexpr bool in_bounds(Position position) noexcept;

  /// @brief The seed this world was constructed from.
  [[nodiscard]] std::uint64_t seed() const noexcept;
  /// @brief The blob's current cell.
  [[nodiscard]] Position position() const noexcept;
  /// @brief Cumulative score from everything eaten so far.
  [[nodiscard]] int score() const noexcept;
  /// @brief The item on a cell, or nullopt if empty or out of bounds.
  [[nodiscard]] std::optional<ItemType>
  item_at(Position position) const noexcept;
  /// @brief Whether the blob has ever stood on or looked at a cell.
  [[nodiscard]] bool is_observed(Position position) const noexcept;
  /// @brief Every remaining item and its position, in row-major order.
  [[nodiscard]] std::vector<ItemPlacement> items() const;
  /// @brief Every observed cell, in row-major order.
  [[nodiscard]] std::vector<Position> observed_positions() const;
  /// @brief How many of an item kind are still on the grid.
  [[nodiscard]] std::size_t remaining_count(ItemType item) const noexcept;
  /// @brief How many of an item kind have been eaten.
  [[nodiscard]] std::size_t eaten_count(ItemType item) const noexcept;

  /// @brief Steps one cell, marking the destination observed.
  /// @return Failure with @c MoveFailure::wall (position unchanged) when the
  /// step would leave the grid. Never collects the destination's item.
  [[nodiscard]] MoveResult move(Direction direction);
  /// @brief Scans every cell from the blob to the wall, marking each
  /// observed.
  [[nodiscard]] LookResult look(Direction direction);
  /// @brief Consumes the item underfoot and applies its reward to the score.
  [[nodiscard]] EatResult eat();

  /// @brief True once no positive-value item remains; toadstools may stay.
  /// The episode's objective-complete condition.
  [[nodiscard]] bool all_positive_items_eaten() const noexcept;

  /// @brief A canonical, row-major representation of all simulation state.
  /// @note Meant for deterministic tests, diagnostics, and replay
  /// comparisons rather than persistence.
  [[nodiscard]] std::string dump() const;

private:
  /// @brief Row-major flat index of a cell.
  [[nodiscard]] static constexpr std::size_t index(Position position) noexcept;
  /// @brief Index of an item kind into the eaten-count array.
  [[nodiscard]] static constexpr std::size_t item_index(ItemType item) noexcept;

  std::uint64_t seed_{};
  Position position_{spawn};
  int score_{};
  std::array<std::optional<ItemType>, cell_count> items_{};
  std::bitset<cell_count> observed_{};
  std::array<std::size_t, 4> eaten_counts_{};
};

constexpr std::string_view direction_name(const Direction direction) noexcept {
  switch (direction) {
  case Direction::north:
    return "north";
  case Direction::south:
    return "south";
  case Direction::east:
    return "east";
  case Direction::west:
    return "west";
  }
  return "unknown";
}

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
