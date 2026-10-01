/// @file world_tools.hpp
/// @brief Typed world actions behind Scry's admission and budget checks.
///
/// No JSON or schema code lives here: scry's reflection layer decodes tool
/// arguments, invokes these typed handlers, and encodes their responses.
#pragma once

#include "agent/config.hpp"
#include "agent/tool_contract.hpp"
#include "world/world.hpp"

namespace pigpen::agent {

/// @brief Owns world semantics and scenario visibility; Scry owns schemas,
/// marshalling, admission, and the per-turn limit.
class WorldTools final {
public:
  explicit WorldTools(world::World &world, const Config &config = {});

  [[nodiscard]] MoveToolResponse move(DirectionArguments arguments);
  /// @note With Config::opaque_look, occupied cells report "something"
  /// instead of the item name.
  [[nodiscard]] LookToolResponse look(DirectionArguments arguments);
  /// @note Reward and score are omitted when Config::reward_feedback is
  /// off; the world still scores truthfully.
  [[nodiscard]] EatToolResponse eat();

private:
  world::World &world_;
  bool opaque_look_;
  bool reward_feedback_;
};

} // namespace pigpen::agent
