/// @file world_tools.hpp
/// @brief Typed world actions behind the tool loop's budget, and the
/// admission policy that the world dispatcher applies before each call.
///
/// Plain C++23: no JSON, schema, or reflection code lives here. The
/// scrydantic toolbox in src/agent decodes tool arguments, calls these
/// actions, and encodes their responses.
#pragma once

#include "core/config.hpp"
#include "core/tool_responses.hpp"
#include "world/world.hpp"

#include <optional>
#include <string_view>

namespace pigpen::core {

/// @brief Owns world semantics and scenario visibility; the binding owns
/// schemas and marshalling, and the tool loop owns the per-turn limit.
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

/// @brief The admission policy for world actions: the refusal text for the
/// model when world actions must stop, else nothing.
///
/// World actions stop after a log failure, or when every positive item is
/// eaten (the objective is complete). A log failure takes priority. The
/// dispatcher asks before each call, so a call later in the same batch is
/// refused too.
/// @param world The world that the actions change.
/// @param logging_failed Whether the episode log failed.
/// @return The exact refusal text, which points to static storage.
[[nodiscard]] std::optional<std::string_view>
admission_refusal(const world::World &world, bool logging_failed);

} // namespace pigpen::core
