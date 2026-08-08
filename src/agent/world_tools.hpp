#pragma once

#include "agent/config.hpp"
#include "agent/tool_contract.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <optional>
#include <utility>

namespace pigpen::agent {

template <typename Response> struct ToolExecution {
  using response_type = Response;

  Response response{};
  world::Position before{};
  world::Position after{};
  std::optional<world::Direction> direction{};
  std::optional<world::ItemType> eaten{};
};

/// Typed application layer behind the reflected model-tool boundary.
///
/// Scry owns JSON Schema generation and strict argument marshalling. This class
/// owns only world semantics and Pig Pen's per-turn action budget.
class WorldTools final {
public:
  explicit WorldTools(world::World &world, Config config = {});

  void begin_turn(std::size_t turn);

  [[nodiscard]] ToolExecution<MoveToolResponse>
  move(DirectionArguments arguments);
  [[nodiscard]] ToolExecution<LookToolResponse>
  look(DirectionArguments arguments);
  [[nodiscard]] ToolExecution<EatToolResponse> eat(EatArguments arguments);

private:
  struct CallPermit {
    bool execute{};
    TurnToolBudget budget{};
  };

  [[nodiscard]] CallPermit begin_call();
  [[nodiscard]] static TurnToolBudget make_budget(std::size_t used);

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
