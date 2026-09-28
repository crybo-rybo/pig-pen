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
direction_name(const Direction direction) noexcept {
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

/// @brief The four edible item kinds placed by the seed.
/// @note Enumerator spelling is protocol-visible through scry and the log.
enum class ItemType : std::uint8_t {
  berry,
  apple,
  truffle,
  toadstool,
};

/// @brief Canonical lowercase name of an item, as shown to the model.
[[nodiscard]] constexpr std::string_view
item_name(const ItemType item) noexcept {
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

/// @brief Score delta for eating an item (toadstools are negative).
[[nodiscard]] constexpr int item_reward(const ItemType item) noexcept {
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

/// @brief Why a move did not change position.
enum class MoveFailure : std::uint8_t {
  wall,
};

/// @brief Why an eat consumed nothing.
enum class EatFailure : std::uint8_t {
  nothing_here,
};

/// @brief Outcome of a single-cell move.
/// @note Moving onto an item never collects it; @c item_here reports what is
/// underfoot so the caller (or the model) can decide to eat.
struct MoveResult {
  bool ok{};
  Position position{};
  std::optional<ItemType> item_here{};
  std::optional<MoveFailure> failure{};
};

/// @brief One cell revealed by a look ray, at @c distance from the blob.
struct LookCell {
  int distance{};
  std::optional<ItemType> item{};
};

/// @brief Every cell from the blob to the wall in one direction.
/// @note @c wall_at_distance is one past the last in-bounds cell, so a blob
/// directly against the wall reports an empty @c cells and distance 1.
struct LookResult {
  std::vector<LookCell> cells{};
  int wall_at_distance{};
};

/// @brief Outcome of eating whatever is underfoot; @c score is cumulative.
struct EatResult {
  bool ok{};
  std::optional<ItemType> ate{};
  int reward{};
  int score{};
  std::optional<EatFailure> failure{};
};

/// @brief An item and where it sits, for omniscient observers (UI, logs).
struct ItemPlacement {
  Position position{};
  ItemType item{};
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

  /// @brief Seeds item placement and marks the spawn cell observed.
  ///
  /// The truffle is placed first, at least 4 Manhattan steps from spawn so
  /// the jackpot is never trivially adjacent; the remaining items draw from
  /// the leftover cells.
  explicit World(std::uint64_t seed);

  /// @brief Whether a position lies inside the walled grid.
  [[nodiscard]] static constexpr bool in_bounds(Position position) noexcept {
    return position.x >= 0 && position.x < width && position.y >= 0 &&
           position.y < height;
  }

  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
  [[nodiscard]] Position position() const noexcept { return position_; }
  /// @brief Cumulative score from everything eaten so far.
  [[nodiscard]] int score() const noexcept { return score_; }
  /// @brief The item on a cell, or nullopt if empty or out of bounds.
  [[nodiscard]] std::optional<ItemType>
  item_at(Position position) const noexcept;
  /// @brief Whether the blob has ever stood on or looked at a cell.
  [[nodiscard]] bool is_observed(Position position) const noexcept;
  /// @brief How many distinct cells have ever been observed, in
  /// [1, cell_count].
  /// @note Counts the spawn cell, which is observed from construction; each
  /// cell counts once however often it is revisited or rescanned.
  [[nodiscard]] std::size_t observed_count() const noexcept {
    return observed_.count();
  }
  /// @brief Every remaining item and its position, in row-major order.
  [[nodiscard]] std::vector<ItemPlacement> items() const;
  /// @brief How many of an item kind have been eaten.
  [[nodiscard]] std::size_t eaten_count(ItemType item) const noexcept {
    return eaten_counts_[static_cast<std::size_t>(item)];
  }

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
  [[nodiscard]] static constexpr std::size_t index(Position position) noexcept {
    return static_cast<std::size_t>(position.y * width + position.x);
  }

  std::uint64_t seed_{};
  Position position_{spawn};
  int score_{};
  std::array<std::optional<ItemType>, cell_count> items_{};
  std::bitset<cell_count> observed_{};
  std::array<std::size_t, 4> eaten_counts_{};
};

} // namespace pigpen::world
