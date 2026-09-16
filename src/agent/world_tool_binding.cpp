/// @file world_tool_binding.cpp
/// @brief Reflected registration and correlation with Scry dispatch results.
#include "agent/world_tool_binding.hpp"

#include <scry/reflection.hpp>

#include <type_traits>
#include <utility>

namespace pigpen::agent {
namespace {

[[nodiscard]] ToolOutcome tool_outcome(const MoveToolResponse &response) {
  return response.reason ? ToolOutcome::blocked_by_wall
                         : ToolOutcome::succeeded;
}

[[nodiscard]] ToolOutcome tool_outcome(const LookToolResponse &) {
  return ToolOutcome::succeeded;
}

[[nodiscard]] ToolOutcome tool_outcome(const EatToolResponse &response) {
  return response.reason ? ToolOutcome::nothing_to_eat : ToolOutcome::succeeded;
}

} // namespace

WorldToolBinding::WorldToolBinding(world::World &world, const Config &config)
    : world_(world), tools_(world, config) {}

template <typename Arguments, typename Invoke>
scry::Status WorldToolBinding::add(scry::ToolRegistry &registry,
                                   const ToolKind kind, std::string description,
                                   Invoke invoke) {
  using Execution = std::invoke_result_t<Invoke &, WorldTools &, Arguments>;
  using Response = typename Execution::response_type;
  return scry::reflection::add<Arguments>(
      registry,
      {.name = std::string{tool_kind_name(kind)},
       .description = std::move(description)},
      [this, kind, invoke](const scry::ToolCallContext &context,
                           Arguments arguments) -> Response {
        auto execution = std::invoke(invoke, tools_, std::move(arguments));
        pending_ = ToolActivity{
            .kind = kind,
            .outcome = tool_outcome(execution.response),
            .arguments_json = "null",
            .result_json = "null",
            .before = execution.before,
            .after = execution.after,
            .direction = execution.direction,
            .eaten = execution.eaten,
            .score_after = world_.score(),
            .scry_turn_id = context.turn_id.value,
            .call_id = std::string{context.call_id},
            .round = context.round,
            .index = context.index,
            .result_dispatched = false,
        };
        return std::move(execution.response);
      });
}

scry::Result<scry::ToolRegistry> WorldToolBinding::registry() {
  scry::ToolRegistry registry;
  if (auto status = add<DirectionArguments>(
          registry, ToolKind::move,
          "Move one cell north, south, east, or west.", &WorldTools::move);
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = add<DirectionArguments>(
          registry, ToolKind::look,
          "Scan every cell in one direction to the wall.", &WorldTools::look);
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = add<EatArguments>(
          registry, ToolKind::eat,
          "Eat the item on the current cell, if present.", &WorldTools::eat);
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return registry;
}

void WorldToolBinding::observe(const scry::ToolCall &call) {
  if (!pending_ || pending_->scry_turn_id != call.turn_id.value ||
      pending_->call_id != call.id) {
    return; // Unknown, undecodable, or refused calls never ran a world action.
  }
  pending_->arguments_json = call.arguments.text;
  pending_->result_json = call.result.text;
  pending_->result_dispatched = true;
  finish_turn();
}

void WorldToolBinding::finish_turn() {
  if (!pending_) {
    return;
  }
  auto activity = std::move(*pending_);
  pending_.reset();
  if (on_activity) {
    on_activity(std::move(activity));
  }
}

} // namespace pigpen::agent
