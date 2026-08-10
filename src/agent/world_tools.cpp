/// @file world_tools.cpp
/// @brief WorldTools implementation; the contract is in the header.
#include "agent/world_tools.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace pigpen::agent {

WorldTools::WorldTools(world::World &world, Config config)
    : world_(world), config_(std::move(config)) {}

void WorldTools::begin_turn(const std::size_t turn) {
  if (active_budget_turn_ && turn < *active_budget_turn_) {
    throw std::logic_error{"world-tool turns must be monotonic"};
  }
  if (!active_budget_turn_ || turn != *active_budget_turn_) {
    active_budget_turn_ = turn;
    tool_calls_used_this_turn_ = 0;
  }
}

ToolExecution<MoveToolResponse>
WorldTools::move(const DirectionArguments arguments) {
  const auto before = world_.position();
  auto permit = begin_call();
  if (!permit.execute) {
    return {
        .response = budget_exhausted(MoveToolResponse{.position = before},
                                     std::move(permit.budget)),
        .before = before,
        .after = before,
        .direction = arguments.direction,
    };
  }

  const auto moved = world_.move(arguments.direction);
  return {
      .response =
          {
              .ok = moved.ok,
              .action_executed = true,
              .item_here = moved.item_here,
              .position = moved.position,
              .reason = moved.failure,
              .turn_tool_budget = std::move(permit.budget),
          },
      .before = before,
      .after = world_.position(),
      .direction = arguments.direction,
  };
}

ToolExecution<LookToolResponse>
WorldTools::look(const DirectionArguments arguments) {
  const auto before = world_.position();
  auto permit = begin_call();
  if (!permit.execute) {
    return {
        .response =
            budget_exhausted(LookToolResponse{.direction = arguments.direction},
                             std::move(permit.budget)),
        .before = before,
        .after = before,
        .direction = arguments.direction,
    };
  }

  const auto looked = world_.look(arguments.direction);
  std::vector<LookToolCell> cells;
  cells.reserve(looked.cells.size());
  for (const auto &cell : looked.cells) {
    std::optional<std::string> item;
    if (cell.item) {
      item = config_.opaque_look ? std::string{"something"}
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
              .action_executed = true,
              .direction = looked.direction,
              .cells = std::move(cells),
              .wall_at_distance = looked.wall_at_distance,
              .turn_tool_budget = std::move(permit.budget),
          },
      .before = before,
      .after = world_.position(),
      .direction = arguments.direction,
  };
}

ToolExecution<EatToolResponse>
WorldTools::eat(const EatArguments /*arguments*/) {
  const auto before = world_.position();
  auto permit = begin_call();
  if (!permit.execute) {
    return {
        .response =
            budget_exhausted(EatToolResponse{}, std::move(permit.budget)),
        .before = before,
        .after = before,
    };
  }

  const auto eaten = world_.eat();
  auto response = EatToolResponse{
      .ok = eaten.ok,
      .action_executed = true,
      .ate = eaten.ate,
      .reason = eaten.failure,
      .turn_tool_budget = std::move(permit.budget),
  };
  if (eaten.ok && config_.reward_feedback) {
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

WorldTools::CallPermit WorldTools::begin_call() {
  if (!active_budget_turn_) {
    throw std::logic_error{"begin_turn must precede world-tool execution"};
  }
  if (tool_calls_used_this_turn_ >= max_world_tool_calls_per_turn) {
    return {
        .execute = false,
        .budget = make_budget(tool_calls_used_this_turn_),
    };
  }
  ++tool_calls_used_this_turn_;
  return {
      .execute = true,
      .budget = make_budget(tool_calls_used_this_turn_),
  };
}

TurnToolBudget WorldTools::make_budget(const std::size_t used) {
  const auto remaining = max_world_tool_calls_per_turn - used;
  return {
      .used = used,
      .remaining = remaining,
      .instruction =
          remaining == 0
              ? "Tool budget exhausted for this turn. Return your final "
                "summary now without calling another tool."
              : std::to_string(remaining) +
                    (remaining == 1 ? " world-tool call remains in this turn."
                                    : " world-tool calls remain in this turn."),
  };
}

} // namespace pigpen::agent
