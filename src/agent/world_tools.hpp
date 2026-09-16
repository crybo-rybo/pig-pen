/// @file world_tools.hpp
/// @brief Typed world actions behind Scry's admission and budget checks.
///
/// No JSON or schema code lives here: scry's reflection layer decodes tool
/// arguments, invokes these typed handlers, and encodes their responses.
#pragma once

#include "agent/config.hpp"
#include "agent/tool_contract.hpp"
#include "world/world.hpp"

#include <optional>

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
/// class owns world semantics and scenario visibility. Scry admits calls and
/// enforces the per-turn limit before invoking any of these actions.
class WorldTools final {
public:
  explicit WorldTools(world::World &world, Config config = {});

  /// @brief Step one cell in a direction.
  [[nodiscard]] ToolExecution<MoveToolResponse>
  move(DirectionArguments arguments);
  /// @brief Scan every cell to the wall in a direction.
  /// @note With Config::opaque_look, occupied cells report "something"
  /// instead of the item name.
  [[nodiscard]] ToolExecution<LookToolResponse>
  look(DirectionArguments arguments);
  /// @brief Consume the item underfoot.
  /// @note Reward and score are omitted when Config::reward_feedback is
  /// off; the world still scores truthfully.
  [[nodiscard]] ToolExecution<EatToolResponse> eat(EatArguments arguments);

private:
  world::World &world_;
  Config config_;
};

} // namespace pigpen::agent
