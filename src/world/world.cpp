/// @file world.cpp
/// @brief Seeded item placement, the three world actions, and the canonical
/// state dump.
#include "world/world.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <random>

namespace pigpen::world {
namespace {

/// @brief The neighbouring position one cell away, ignoring bounds.
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

/// @brief City-block distance used to keep the truffle away from spawn.
[[nodiscard]] int manhattan_distance(const Position lhs,
                                     const Position rhs) noexcept {
  return std::abs(lhs.x - rhs.x) + std::abs(lhs.y - rhs.y);
}

/// @brief Uniform draw in [0, bound) by rejection sampling.
///
/// std::uniform_int_distribution does not promise the same mapping on every
/// standard library. Keeping the bounded draw here makes a seed portable
/// across supported compilers and still avoids modulo bias.
[[nodiscard]] std::size_t bounded_index(std::mt19937_64 &engine,
                                        const std::size_t bound) {
  const auto unsigned_bound = static_cast<std::uint64_t>(bound);
  const auto rejection_threshold = -unsigned_bound % unsigned_bound;
  while (true) {
    const auto value = engine();
    if (value >= rejection_threshold) {
      return static_cast<std::size_t>(value % unsigned_bound);
    }
  }
}

/// @brief Removes and returns a uniformly chosen candidate cell.
[[nodiscard]] Position take_random(std::vector<Position> &candidates,
                                   std::mt19937_64 &engine) {
  const auto selected = bounded_index(engine, candidates.size());
  const auto position = candidates[selected];
  candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(selected));
  return position;
}

/// @brief One-character cell glyph for dump(): '.' empty, item initial, or
/// 'x' for a toadstool.
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
  std::erase(candidates, truffle_position);

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

MoveResult World::move(const Direction direction) {
  const auto destination = step(position_, direction);
  if (!in_bounds(destination)) {
    return {.position = position_, .failure = MoveFailure::wall};
  }

  position_ = destination;
  observed_.set(index(position_));
  return {.ok = true, .position = position_, .item_here = item_at(position_)};
}

LookResult World::look(const Direction direction) {
  LookResult result;
  auto scanned = step(position_, direction);
  int distance = 1;
  while (in_bounds(scanned)) {
    observed_.set(index(scanned));
    result.cells.push_back({.distance = distance, .item = item_at(scanned)});
    scanned = step(scanned, direction);
    ++distance;
  }
  result.wall_at_distance = distance;
  return result;
}

EatResult World::eat() {
  const auto item = item_at(position_);
  if (!item) {
    return {.score = score_, .failure = EatFailure::nothing_here};
  }

  const auto reward = item_reward(*item);
  score_ += reward;
  items_[index(position_)] = std::nullopt;
  ++eaten_counts_[static_cast<std::size_t>(*item)];
  return {.ok = true, .ate = item, .reward = reward, .score = score_};
}

bool World::all_positive_items_eaten() const noexcept {
  return std::none_of(items_.begin(), items_.end(), [](const auto item) {
    return item && item_reward(*item) > 0;
  });
}

std::string World::dump() const {
  auto out = std::format("seed={};position={},{};score={};items=", seed_,
                         position_.x, position_.y, score_);
  for (const auto item : items_) {
    out += item_glyph(item);
  }
  out += ";observed=";
  for (std::size_t cell = 0; cell < cell_count; ++cell) {
    out += observed_.test(cell) ? '1' : '0';
  }
  out += std::format(";eaten={},{},{},{}", eaten_counts_[0], eaten_counts_[1],
                     eaten_counts_[2], eaten_counts_[3]);
  return out;
}

} // namespace pigpen::world
