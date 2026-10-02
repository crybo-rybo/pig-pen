/// @file world_tools.cpp
/// @brief WorldTools implementation; the contract is in the header.
#include "core/world_tools.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pigpen::core {

WorldTools::WorldTools(world::World &world, const Config &config)
    : world_(world), opaque_look_(config.opaque_look),
      reward_feedback_(config.reward_feedback) {}

MoveToolResponse WorldTools::move(const world::Direction direction) {
  const auto moved = world_.move(direction);
  return {
      .ok = moved.ok,
      .item_here = moved.item_here,
      .position = moved.position,
      .reason = moved.failure,
  };
}

LookToolResponse WorldTools::look(const world::Direction direction) {
  const auto looked = world_.look(direction);
  std::vector<LookToolCell> cells;
  cells.reserve(looked.cells.size());
  for (const auto &cell : looked.cells) {
    std::optional<std::string> item;
    if (cell.item) {
      item = opaque_look_ ? std::string{"something"}
                          : std::string{world::item_name(*cell.item)};
    }
    cells.push_back({
        .distance = cell.distance,
        .item = std::move(item),
    });
  }
  return {
      .ok = true,
      .direction = direction,
      .cells = std::move(cells),
      .wall_at_distance = looked.wall_at_distance,
  };
}

EatToolResponse WorldTools::eat() {
  const auto eaten = world_.eat();
  EatToolResponse response{
      .ok = eaten.ok,
      .ate = eaten.ate,
      .reason = eaten.failure,
  };
  if (eaten.ok && reward_feedback_) {
    response.reward = eaten.reward;
    response.score = eaten.score;
  }
  return response;
}

} // namespace pigpen::core
