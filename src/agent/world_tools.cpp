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
        .response = budget_exhausted<MoveToolResult>(std::move(permit.budget)),
        .before = before,
        .after = before,
        .direction = arguments.direction,
        .action_executed = false,
    };
  }

  const auto moved = world_.move(arguments.direction);
  return {
      .response = completed(
          MoveToolResult{
              .item_here = moved.item_here,
              .ok = moved.ok,
              .position = moved.position,
              .reason = moved.failure,
          },
          std::move(permit.budget)),
      .before = before,
      .after = world_.position(),
      .direction = arguments.direction,
      .action_executed = true,
  };
}

ToolExecution<LookToolResponse>
WorldTools::look(const DirectionArguments arguments) {
  const auto before = world_.position();
  auto permit = begin_call();
  if (!permit.execute) {
    return {
        .response = budget_exhausted<LookToolResult>(std::move(permit.budget)),
        .before = before,
        .after = before,
        .direction = arguments.direction,
        .action_executed = false,
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
      .response = completed(
          LookToolResult{
              .cells = std::move(cells),
              .direction = looked.direction,
              .wall_at_distance = looked.wall_at_distance,
          },
          std::move(permit.budget)),
      .before = before,
      .after = world_.position(),
      .direction = arguments.direction,
      .action_executed = true,
  };
}

ToolExecution<EatToolResponse>
WorldTools::eat(const EatArguments /*arguments*/) {
  const auto before = world_.position();
  auto permit = begin_call();
  if (!permit.execute) {
    return {
        .response = budget_exhausted<EatToolResult>(std::move(permit.budget)),
        .before = before,
        .after = before,
        .action_executed = false,
    };
  }

  const auto eaten = world_.eat();
  auto result = EatToolResult{
      .ate = eaten.ate,
      .ok = eaten.ok,
      .reason = eaten.failure,
  };
  if (eaten.ok && config_.reward_feedback) {
    result.reward = eaten.reward;
    result.score = eaten.score;
  }

  return {
      .response = completed(std::move(result), std::move(permit.budget)),
      .before = before,
      .after = world_.position(),
      .action_executed = true,
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
