/// @file world_tools.cpp
/// @brief WorldTools implementation; the contract is in the header.
#include "agent/world_tools.hpp"

#include <string>
#include <utility>

namespace pigpen::agent {

WorldTools::WorldTools(world::World &world, const Config &config)
    : world_(world), opaque_look_(config.opaque_look),
      reward_feedback_(config.reward_feedback) {}

ToolExecution<MoveToolResponse>
WorldTools::move(const DirectionArguments arguments) {
  const auto before = world_.position();

  const auto moved = world_.move(arguments.direction);
  return {
      .response =
          {
              .ok = moved.ok,
              .item_here = moved.item_here,
              .position = moved.position,
              .reason = moved.failure,
          },
      .before = before,
      .after = world_.position(),
      .direction = arguments.direction,
  };
}

ToolExecution<LookToolResponse>
WorldTools::look(const DirectionArguments arguments) {
  const auto before = world_.position();

  const auto looked = world_.look(arguments.direction);
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
      .response =
          {
              .ok = true,
              .direction = arguments.direction,
              .cells = std::move(cells),
              .wall_at_distance = looked.wall_at_distance,
          },
      .before = before,
      .after = world_.position(),
      .direction = arguments.direction,
  };
}

ToolExecution<EatToolResponse>
WorldTools::eat(const EatArguments /*arguments*/) {
  const auto before = world_.position();

  const auto eaten = world_.eat();
  auto response = EatToolResponse{
      .ok = eaten.ok,
      .ate = eaten.ate,
      .reason = eaten.failure,
  };
  if (eaten.ok && reward_feedback_) {
    response.reward = eaten.reward;
    response.score = eaten.score;
  }

  return {
      .response = std::move(response),
      .before = before,
      .after = world_.position(),
      .eaten = eaten.ate,
  };
}

} // namespace pigpen::agent
