/// @file world_tool_controller.cpp
/// @brief WorldToolController implementation; the contract is in the header.
#include "agent/world_tool_controller.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief "(x, y)" rendering shared by all summaries.
[[nodiscard]] std::string position_text(const world::Position position) {
  return "(" + std::to_string(position.x) + ", " + std::to_string(position.y) +
         ")";
}

/// @brief Budget rejection reads the same for every tool.
[[nodiscard]] std::string
budget_summary(const ToolKind kind,
               const std::optional<world::Direction> direction) {
  auto summary = std::string{tool_kind_name(kind)};
  if (direction) {
    summary += ' ';
    summary += world::direction_name(*direction);
  }
  summary += " rejected: turn tool budget exhausted";
  return summary;
}

} // namespace

WorldToolController::WorldToolController(WorldTools &tools,
                                         IToolActivitySink &sink,
                                         TurnProvider current_turn)
    : tools_(tools), sink_(sink), current_turn_(std::move(current_turn)) {}

MoveToolResponse WorldToolController::move(const DirectionArguments arguments) {
  const auto turn = current_turn_();
  tools_.begin_turn(turn);
  auto execution = tools_.move(arguments);

  auto outcome = ToolOutcome::succeeded;
  std::string summary;
  const auto direction = world::direction_name(arguments.direction);
  if (!execution.response.action_executed) {
    outcome = ToolOutcome::budget_exhausted;
    summary = budget_summary(ToolKind::move, arguments.direction);
  } else if (execution.response.reason) {
    // The world enumerates every failure; -Wswitch turns a new enumerator
    // into a compile error here instead of a silently wrong outcome.
    switch (*execution.response.reason) {
    case world::MoveFailure::wall:
      outcome = ToolOutcome::blocked_by_wall;
      summary = "move " + std::string{direction} + " blocked by the wall at " +
                position_text(execution.before);
      break;
    }
  } else {
    summary = "move " + std::string{direction} + ": " +
              position_text(execution.before) + " -> " +
              position_text(execution.after);
  }

  publish(turn, ToolKind::move, execution, outcome, std::move(summary));
  return std::move(execution.response);
}

LookToolResponse WorldToolController::look(const DirectionArguments arguments) {
  const auto turn = current_turn_();
  tools_.begin_turn(turn);
  auto execution = tools_.look(arguments);

  auto outcome = ToolOutcome::succeeded;
  std::string summary;
  const auto direction = world::direction_name(arguments.direction);
  if (!execution.response.action_executed) {
    outcome = ToolOutcome::budget_exhausted;
    summary = budget_summary(ToolKind::look, arguments.direction);
  } else {
    // Occupied-cell count is truthful even under opaque_look; only item
    // identity is masked from the model.
    const auto occupied = std::ranges::count_if(
        execution.response.cells,
        [](const LookToolCell &cell) { return cell.item.has_value(); });
    summary = "look " + std::string{direction} + ": " +
              std::to_string(occupied) + " occupied of " +
              std::to_string(execution.response.cells.size()) +
              " cells, wall at distance " +
              std::to_string(execution.response.wall_at_distance);
  }

  publish(turn, ToolKind::look, execution, outcome, std::move(summary));
  return std::move(execution.response);
}

EatToolResponse WorldToolController::eat(const EatArguments arguments) {
  const auto turn = current_turn_();
  tools_.begin_turn(turn);
  auto execution = tools_.eat(arguments);

  auto outcome = ToolOutcome::succeeded;
  std::string summary;
  if (!execution.response.action_executed) {
    outcome = ToolOutcome::budget_exhausted;
    summary = budget_summary(ToolKind::eat, std::nullopt);
  } else if (execution.response.reason) {
    // Exhaustive for the same reason as the move failure switch above.
    switch (*execution.response.reason) {
    case world::EatFailure::nothing_here:
      outcome = ToolOutcome::nothing_to_eat;
      summary = "eat: nothing here at " + position_text(execution.before);
      break;
    }
  } else {
    // A successful eat always carries the item (world contract). The reward
    // is derived from the world's item table, so the summary and log stay
    // truthful even when reward feedback is hidden from the model.
    const auto item = *execution.eaten;
    const auto reward = world::item_reward(item);
    summary = "ate " + std::string{world::item_name(item)} + " (" +
              (reward >= 0 ? "+" : "") + std::to_string(reward) + ") at " +
              position_text(execution.before);
  }

  publish(turn, ToolKind::eat, execution, outcome, std::move(summary));
  return std::move(execution.response);
}

template <typename Response>
void WorldToolController::publish(const std::size_t turn, const ToolKind kind,
                                  const ToolExecution<Response> &execution,
                                  const ToolOutcome outcome,
                                  std::string summary) {
  sink_.publish({
      .tick = 0, // Stamped by the journal.
      .turn = turn,
      .kind = kind,
      .outcome = outcome,
      .before = execution.before,
      .after = execution.after,
      .direction = execution.direction,
      .eaten = execution.eaten,
      .score_after = tools_.world().score(),
      .summary = std::move(summary),
  });
}

} // namespace pigpen::agent
