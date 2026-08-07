#include "world/world.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <locale>
#include <random>
#include <sstream>
#include <utility>

namespace pigpen::world {
namespace {

[[nodiscard]] Position step(const Position position,
                            const Direction direction) noexcept {
  switch (direction) {
  case Direction::north:
    return {.x = position.x, .y = position.y + 1};
  case Direction::south:
    return {.x = position.x, .y = position.y - 1};
  case Direction::east:
    return {.x = position.x + 1, .y = position.y};
  case Direction::west:
    return {.x = position.x - 1, .y = position.y};
  }
  return position;
}

[[nodiscard]] int manhattan_distance(const Position lhs,
                                     const Position rhs) noexcept {
  return std::abs(lhs.x - rhs.x) + std::abs(lhs.y - rhs.y);
}

// std::uniform_int_distribution does not promise the same mapping on every
// standard library. Keeping the bounded draw here makes a seed portable across
// supported compilers and still avoids modulo bias.
[[nodiscard]] std::size_t bounded_index(std::mt19937_64 &engine,
                                        const std::size_t bound) {
  const auto unsigned_bound = static_cast<std::uint64_t>(bound);
  const auto rejection_threshold =
      static_cast<std::uint64_t>(-unsigned_bound) % unsigned_bound;
  while (true) {
    const auto value = engine();
    if (value >= rejection_threshold) {
      return static_cast<std::size_t>(value % unsigned_bound);
    }
  }
}

[[nodiscard]] Position take_random(std::vector<Position> &candidates,
                                   std::mt19937_64 &engine) {
  const auto selected = bounded_index(engine, candidates.size());
  const auto position = candidates[selected];
  candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(selected));
  return position;
}

[[nodiscard]] char item_glyph(const std::optional<ItemType> item) noexcept {
  if (!item) {
    return '.';
  }
  switch (*item) {
  case ItemType::berry:
    return 'b';
  case ItemType::apple:
    return 'a';
  case ItemType::truffle:
    return 't';
  case ItemType::toadstool:
    return 'x';
  }
  return '?';
}

} // namespace

World::World(const std::uint64_t seed) : seed_(seed) {
  observed_.set(index(spawn));

  std::vector<Position> candidates;
  candidates.reserve(cell_count - 1);
  std::vector<Position> truffle_candidates;
  truffle_candidates.reserve(cell_count - 1);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Position position{.x = x, .y = y};
      if (position == spawn) {
        continue;
      }
      candidates.push_back(position);
      if (manhattan_distance(position, spawn) >= 4) {
        truffle_candidates.push_back(position);
      }
    }
  }

  std::mt19937_64 engine{seed_};

  const auto truffle_position = take_random(truffle_candidates, engine);
  items_[index(truffle_position)] = ItemType::truffle;
  const auto truffle_in_candidates =
      std::find(candidates.begin(), candidates.end(), truffle_position);
  candidates.erase(truffle_in_candidates);

  const auto place = [this, &candidates, &engine](const ItemType item,
                                                  const std::size_t count) {
    for (std::size_t placed = 0; placed < count; ++placed) {
      const auto position = take_random(candidates, engine);
      items_[index(position)] = item;
    }
  };

  place(ItemType::berry, default_berry_count);
  place(ItemType::apple, default_apple_count);
  place(ItemType::toadstool, default_toadstool_count);
}

std::uint64_t World::seed() const noexcept { return seed_; }

Position World::position() const noexcept { return position_; }

int World::score() const noexcept { return score_; }

std::optional<ItemType> World::item_at(const Position position) const noexcept {
  if (!in_bounds(position)) {
    return std::nullopt;
  }
  return items_[index(position)];
}

bool World::is_observed(const Position position) const noexcept {
  return in_bounds(position) && observed_.test(index(position));
}

std::vector<ItemPlacement> World::items() const {
  std::vector<ItemPlacement> placements;
  placements.reserve(default_item_count);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Position position{.x = x, .y = y};
      if (const auto item = item_at(position)) {
        placements.push_back({.position = position, .item = *item});
      }
    }
  }
  return placements;
}

std::vector<Position> World::observed_positions() const {
  std::vector<Position> positions;
  positions.reserve(observed_.count());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Position position{.x = x, .y = y};
      if (is_observed(position)) {
        positions.push_back(position);
      }
    }
  }
  return positions;
}

std::size_t World::remaining_count(const ItemType item) const noexcept {
  return static_cast<std::size_t>(
      std::count(items_.begin(), items_.end(), item));
}

std::size_t World::eaten_count(const ItemType item) const noexcept {
  return eaten_counts_[item_index(item)];
}

MoveResult World::move(const Direction direction) {
  const auto destination = step(position_, direction);
  if (!in_bounds(destination)) {
    return {
        .ok = false,
        .position = position_,
        .item_here = std::nullopt,
        .failure = MoveFailure::wall,
    };
  }

  position_ = destination;
  observed_.set(index(position_));
  return {
      .ok = true,
      .position = position_,
      .item_here = item_at(position_),
      .failure = std::nullopt,
  };
}

LookResult World::look(const Direction direction) {
  LookResult result{.direction = direction};
  auto scanned = step(position_, direction);
  int distance = 1;
  while (in_bounds(scanned)) {
    observed_.set(index(scanned));
    result.cells.push_back({
        .distance = distance,
        .position = scanned,
        .item = item_at(scanned),
    });
    scanned = step(scanned, direction);
    ++distance;
  }
  result.wall_at_distance = distance;
  return result;
}

EatResult World::eat() {
  const auto item = item_at(position_);
  if (!item) {
    return {
        .ok = false,
        .ate = std::nullopt,
        .reward = 0,
        .score = score_,
        .failure = EatFailure::nothing_here,
    };
  }

  const auto reward = item_reward(*item);
  score_ += reward;
  items_[index(position_)] = std::nullopt;
  ++eaten_counts_[item_index(*item)];
  return {
      .ok = true,
      .ate = item,
      .reward = reward,
      .score = score_,
      .failure = std::nullopt,
  };
}

bool World::all_positive_items_eaten() const noexcept {
  return std::none_of(items_.begin(), items_.end(), [](const auto item) {
    return item && item_reward(*item) > 0;
  });
}

std::string World::dump() const {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "seed=" << seed_ << ";position=" << position_.x << ','
         << position_.y << ";score=" << score_ << ";items=";
  for (const auto item : items_) {
    output << item_glyph(item);
  }
  output << ";observed=";
  for (std::size_t cell = 0; cell < cell_count; ++cell) {
    output << (observed_.test(cell) ? '1' : '0');
  }
  output << ";eaten=" << eaten_count(ItemType::berry) << ','
         << eaten_count(ItemType::apple) << ','
         << eaten_count(ItemType::truffle) << ','
         << eaten_count(ItemType::toadstool);
  return output.str();
}

} // namespace pigpen::world
