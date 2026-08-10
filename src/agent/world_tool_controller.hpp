/// @file world_tool_controller.hpp
/// @brief The application service behind the reflected tool boundary.
///
/// Scry decodes model tool calls into typed arguments and encodes typed
/// responses; WorldToolController is what those decoded calls land on. Each
/// handler opens the per-turn budget window, invokes WorldTools, publishes
/// exactly one typed ToolActivity, and returns only the typed response.
/// No JSON exists at this layer.
#pragma once

#include "agent/tool_activity.hpp"
#include "agent/tool_contract.hpp"
#include "agent/world_tools.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace pigpen::agent {

/// @brief Per-call orchestration for the three world tools.
///
/// The controller is deliberately free of registration, networking, and
/// persistence concerns: Session registers thin reflected callables that
/// forward here, and the sink (normally the ToolActivityJournal) fans the
/// published activity out to the UI, animation, and metrics.
class WorldToolController final {
public:
  /// @brief Supplies the 1-based turn a decoded call belongs to.
  using TurnProvider = std::function<std::size_t()>;

  /// @param tools Typed world actions plus the per-turn action budget; the
  /// same tools also lend the world view used for truthful score facts.
  /// @param sink Receives exactly one activity per decoded call.
  /// @param current_turn Invoked at dispatch time, not construction time.
  WorldToolController(WorldTools &tools, IToolActivitySink &sink,
                      TurnProvider current_turn);

  /// @brief Handle a decoded `move` call and publish its activity.
  [[nodiscard]] MoveToolResponse move(DirectionArguments arguments);
  /// @brief Handle a decoded `look` call and publish its activity.
  [[nodiscard]] LookToolResponse look(DirectionArguments arguments);
  /// @brief Handle a decoded `eat` call and publish its activity.
  [[nodiscard]] EatToolResponse eat(EatArguments arguments);

private:
  template <typename Response>
  void publish(std::size_t turn, ToolKind kind,
               const ToolExecution<Response> &execution, ToolOutcome outcome,
               std::string summary);

  WorldTools &tools_;
  IToolActivitySink &sink_;
  TurnProvider current_turn_;
};

} // namespace pigpen::agent
