/// @file world_tools.hpp
/// @brief Typed world actions and the per-turn action-budget lifecycle.
///
/// No JSON or schema code lives here: scry's reflection layer decodes tool
/// arguments, invokes these typed handlers, and encodes their responses.
#pragma once

#include "agent/config.hpp"
#include "agent/tool_contract.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <optional>
#include <utility>

namespace pigpen::agent {

/// @brief A tool's typed response plus the facts the application observes:
/// before/after positions, direction, and any eaten item feed the event
/// log, animation, and metrics without re-deriving them from the response.
template <typename Response> struct ToolExecution {
  using response_type = Response;

  Response response{};
  world::Position before{};
  world::Position after{};
  std::optional<world::Direction> direction{};
  std::optional<world::ItemType> eaten{};
};

/// @brief Typed application layer behind the reflected model-tool boundary.
///
/// Scry owns JSON Schema generation and strict argument marshalling. This
/// class owns only world semantics and Pig Pen's per-turn action budget: a
/// call beyond max_world_tool_calls_per_turn still returns a well-formed
/// response, but with action_executed == false and the world untouched.
class WorldTools final {
public:
  explicit WorldTools(world::World &world, Config config = {});

  /// @brief Open the budget window for @p turn; advancing the turn resets
  /// the per-turn call counter.
  /// @note Throws std::logic_error when @p turn moves backwards — turns
  /// must be monotonic.
  void begin_turn(std::size_t turn);

  /// @brief Step one cell in a direction, budget permitting.
  [[nodiscard]] ToolExecution<MoveToolResponse>
  move(DirectionArguments arguments);
  /// @brief Scan every cell to the wall in a direction, budget permitting.
  /// @note With Config::opaque_look, occupied cells report "something"
  /// instead of the item name.
  [[nodiscard]] ToolExecution<LookToolResponse>
  look(DirectionArguments arguments);
  /// @brief Consume the item underfoot, budget permitting.
  /// @note Reward and score are omitted when Config::reward_feedback is
  /// off; the world still scores truthfully.
  [[nodiscard]] ToolExecution<EatToolResponse> eat(EatArguments arguments);

  /// @brief Read-only view of the world these tools mutate, so callers can
  /// report truthful facts about the same simulation.
  [[nodiscard]] const world::World &world() const noexcept { return world_; }

private:
  /// @brief Outcome of charging the budget for one call.
  struct CallPermit {
    bool execute{};
    TurnToolBudget budget{};
  };

  [[nodiscard]] CallPermit begin_call();
  [[nodiscard]] static TurnToolBudget make_budget(std::size_t used);

  /// @brief Shape @p response into the fixed budget-exhausted envelope.
  template <typename Response>
  [[nodiscard]] static Response budget_exhausted(Response response,
                                                 TurnToolBudget budget) {
    response.ok = false;
    response.action_executed = false;
    response.error_code = ToolFailureCode::tool_budget_exhausted;
    response.error =
        "No action was executed because this turn's world-tool call budget "
        "is exhausted.";
    response.turn_tool_budget = std::move(budget);
    return response;
  }

  world::World &world_;
  Config config_;
  std::optional<std::size_t> active_budget_turn_{};
  std::size_t tool_calls_used_this_turn_{};
};

} // namespace pigpen::agent
