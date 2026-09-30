/// @file world_tool_binding.cpp
/// @brief Reflected registration and correlation with Scry dispatch results.
#include "agent/world_tool_binding.hpp"

#include <scry/reflection.hpp>

#include <functional>
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
  return registry.add<Arguments>(
      {.name = std::string{tool_kind_name(kind)},
       .description = std::move(description)},
      [this, kind, invoke](const scry::ToolCallContext &context,
                           const Arguments arguments) {
        const auto before = world_.position();
        auto response = std::invoke(invoke, tools_, arguments);
        pending_ = ToolActivity{
            .kind = kind,
            .outcome = tool_outcome(response),
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
        };
        if constexpr (std::is_same_v<Arguments, DirectionArguments>) {
          pending_->direction = arguments.direction;
        }
        if constexpr (std::is_same_v<decltype(response), EatToolResponse>) {
          pending_->eaten = response.ate;
        }
        return response;
      });
}

scry::Result<scry::ToolRegistry> WorldToolBinding::registry() {
  scry::ToolRegistry registry;
  auto added =
      add<DirectionArguments>(registry, ToolKind::move,
                              "Move one cell north, south, east, or west.",
                              &WorldTools::move)
          .and_then([&] {
            return add<DirectionArguments>(
                registry, ToolKind::look,
                "Scan every cell in one direction to the wall.",
                &WorldTools::look);
          })
          .and_then([&] {
            return add<EatArguments>(
                registry, ToolKind::eat,
                "Eat the item on the current cell, if present.",
                &WorldTools::eat);
          });
  if (!added) {
    return std::unexpected(std::move(added.error()));
  }
  return registry;
}

std::optional<scry::ToolRejection>
WorldToolBinding::admit(const bool logging_failed) {
  if (logging_failed) {
    ++host_refused_calls_;
    return scry::ToolRejection{
        .model_message = "World tools are unavailable because the episode log "
                         "failed. Summarize the actions already taken."};
  }
  if (world_.all_positive_items_eaten()) {
    ++host_refused_calls_;
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

std::uint32_t WorldToolBinding::complete_turn() {
  flush_pending_activity();
  return std::exchange(host_refused_calls_, 0U);
}

} // namespace pigpen::agent
