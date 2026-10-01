/// @file world_tool_binding.cpp
/// @brief Toolbox registration and correlation with Scry dispatch results.
#include "agent/world_tool_binding.hpp"

#include <memory>
#include <utility>

namespace pigpen::agent {

WorldToolBinding::WorldToolBinding(world::World &world, const Config &config)
    : world_(world), tools_(world, config) {}

scry::Result<scry::ToolRegistry> WorldToolBinding::registry() {
  scry::ToolRegistry registry;
  // The session owns this binding and outlives the harness, so the registry
  // borrows it through a non-owning pointer.
  auto added = registry.add(
      std::shared_ptr<WorldToolBinding>{this, [](WorldToolBinding *) {}});
  if (!added) {
    return std::unexpected(std::move(added.error()));
  }
  return registry;
}

MoveToolResponse WorldToolBinding::move(const scry::ToolCallContext &context,
                                        const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.move(arguments);
  stage(context, ToolKind::move,
        response.reason ? ToolOutcome::blocked_by_wall : ToolOutcome::succeeded,
        before)
      .direction = arguments.direction;
  return response;
}

LookToolResponse WorldToolBinding::look(const scry::ToolCallContext &context,
                                        const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.look(arguments);
  stage(context, ToolKind::look, ToolOutcome::succeeded, before).direction =
      arguments.direction;
  return response;
}

EatToolResponse WorldToolBinding::eat(const scry::ToolCallContext &context) {
  const auto before = world_.position();
  auto response = tools_.eat();
  stage(context, ToolKind::eat,
        response.reason ? ToolOutcome::nothing_to_eat : ToolOutcome::succeeded,
        before)
      .eaten = response.ate;
  return response;
}

ToolActivity &WorldToolBinding::stage(const scry::ToolCallContext &context,
                                      const ToolKind kind,
                                      const ToolOutcome outcome,
                                      const world::Position before) {
  return pending_.emplace(ToolActivity{
      .kind = kind,
      .outcome = outcome,
      .arguments_json = "null",
      .result_json = "null",
      .before = before,
      .after = world_.position(),
      .score_after = world_.score(),
      .scry_turn_id = context.turn_id.value,
      .call_id = std::string{context.call_id},
      .round = context.round,
      .index = context.index,
      .result_dispatched = false,
  });
}

std::optional<scry::ToolRejection>
WorldToolBinding::admit(const bool logging_failed) const {
  if (logging_failed) {
    return scry::ToolRejection{
        .model_message = "World tools are unavailable because the episode log "
                         "failed. Summarize the actions already taken."};
  }
  if (world_.all_positive_items_eaten()) {
    return scry::ToolRejection{
        .model_message = "All positive-value items have been eaten. Summarize "
                         "the completed episode without more tools."};
  }
  return std::nullopt;
}

void WorldToolBinding::observe(const scry::ToolCall &call) {
  if (!pending_ || pending_->scry_turn_id != call.turn_id.value ||
      pending_->call_id != call.id) {
    return; // Unknown, undecodable, or refused calls never ran a world action.
  }
  pending_->arguments_json = call.arguments.text;
  pending_->result_json = call.result.text;
  pending_->result_dispatched = true;
  flush_pending_activity();
}

void WorldToolBinding::flush_pending_activity() {
  auto activity = std::exchange(pending_, std::nullopt);
  if (activity && on_activity) {
    on_activity(std::move(*activity));
  }
}

} // namespace pigpen::agent
