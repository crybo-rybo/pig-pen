/// @file world_tool_binding.cpp
/// @brief Toolbox registration and correlation with Scry dispatch results.
#include "agent/world_tool_binding.hpp"

#include <utility>

namespace pigpen::agent {

WorldToolBinding::WorldToolBinding(world::World &world,
                                   const core::Config &config)
    : world_(world), tools_(world, config) {}

// Out-of-line destruction avoids GCC 16.0.1's -O3 speculative devirtualization
// diagnosing this destructor for unrelated shared_ptr control blocks, such as
// nlohmann/json's assertion diagnostics in the reflection tests.
WorldToolBinding::~WorldToolBinding() = default;

core::MoveToolResponse
WorldToolBinding::move(const scry::ToolCallContext &context,
                       const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.move(arguments.direction);
  stage(context, core::ToolKind::move,
        response.reason ? core::ToolOutcome::blocked_by_wall
                        : core::ToolOutcome::succeeded,
        before)
      .direction = arguments.direction;
  return response;
}

core::LookToolResponse
WorldToolBinding::look(const scry::ToolCallContext &context,
                       const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.look(arguments.direction);
  stage(context, core::ToolKind::look, core::ToolOutcome::succeeded, before)
      .direction = arguments.direction;
  return response;
}

core::EatToolResponse
WorldToolBinding::eat(const scry::ToolCallContext &context) {
  const auto before = world_.position();
  auto response = tools_.eat();
  stage(context, core::ToolKind::eat,
        response.reason ? core::ToolOutcome::nothing_to_eat
                        : core::ToolOutcome::succeeded,
        before)
      .eaten = response.ate;
  return response;
}

core::ToolActivity &WorldToolBinding::stage(
    const scry::ToolCallContext &context, const core::ToolKind kind,
    const core::ToolOutcome outcome, const world::Position before) {
  return pending_.emplace(core::ToolActivity{
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
