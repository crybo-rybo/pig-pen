/// @file world_tools.hpp
/// @brief Typed world actions behind Scry's admission and budget checks.
///
/// Plain C++23: no JSON, schema, or reflection code lives here. The Scry
/// toolbox in agent/world_tool_binding.hpp decodes tool arguments, calls these
/// actions, and encodes their responses.
#pragma once

#include "core/config.hpp"
#include "core/tool_responses.hpp"
#include "world/world.hpp"

namespace pigpen::core {

/// @brief Owns world semantics and scenario visibility; Scry owns schemas,
/// marshalling, admission, and the per-turn limit.
class WorldTools final {
public:
  explicit WorldTools(world::World &world, const Config &config = {});

  [[nodiscard]] MoveToolResponse move(world::Direction direction);
  /// @note With Config::opaque_look, occupied cells report "something"
  /// instead of the item name.
  [[nodiscard]] LookToolResponse look(world::Direction direction);
  /// @note Reward and score are omitted when Config::reward_feedback is
  /// off; the world still scores truthfully.
  [[nodiscard]] EatToolResponse eat();

private:
  world::World &world_;
  bool opaque_look_;
  bool reward_feedback_;
};

} // namespace pigpen::core
