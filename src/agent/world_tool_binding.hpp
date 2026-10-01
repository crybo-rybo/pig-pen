/// @file world_tool_binding.hpp
/// @brief The world tools as a Scry toolbox, and the correlation of each
/// world transition with the exact payload Scry dispatched.
#pragma once

#include "agent/tool_contract.hpp"
#include "core/events.hpp"
#include "core/world_tools.hpp"

#include <scry/scry.hpp>

#include <functional>
#include <optional>

namespace pigpen::agent {

/// @brief Scry toolbox whose annotated members are the model's world tools.
///
/// Scry names each tool after its member function and generates its schema,
/// argument decoder, and result encoder from the declaration. Each member
/// runs the typed world action and stages its ToolActivity; observe() then
/// joins it to the canonical payload Scry dispatched.
/// @note Must outlive the single harness adopting registry(). Dispatch and
/// observation are serial on the pump thread, so only one transition can be
/// pending.
class WorldToolBinding final {
public:
  WorldToolBinding(world::World &world, const core::Config &config);
  WorldToolBinding(const WorldToolBinding &) = delete;
  WorldToolBinding &operator=(const WorldToolBinding &) = delete;

  /// A registry holding this toolbox, borrowed rather than owned.
  [[nodiscard]] scry::Result<scry::ToolRegistry> registry();
  /// Refuse further world actions after a log failure or objective completion.
  [[nodiscard]] std::optional<scry::ToolRejection>
  admit(bool logging_failed) const;
  void observe(const scry::ToolCall &call);
  /// Publish any retained world side effect once. Call on turn completion
  /// and before destroying the session's activity sink; normal dispatch
  /// already flushes through observe().
  void flush_pending_activity();

  // clang-format off: keep each P3394 annotation on its own line.
  [[= scry::reflection::tool{"Move one cell north, south, east, or west."}]]
  core::MoveToolResponse move(const scry::ToolCallContext &context,
                        DirectionArguments arguments);
  [[= scry::reflection::tool{"Scan every cell in one direction to the wall."}]]
  core::LookToolResponse look(const scry::ToolCallContext &context,
                        DirectionArguments arguments);
  [[= scry::reflection::tool{"Eat the item on the current cell, if present."}]]
  core::EatToolResponse eat(const scry::ToolCallContext &context);
  // clang-format on

  /// Called only for decoded, admitted world actions.
  std::function<void(core::ToolActivity)> on_activity{};

private:
  core::ToolActivity &stage(const scry::ToolCallContext &context,
                            core::ToolKind kind, core::ToolOutcome outcome,
                            world::Position before);

  world::World &world_;
  core::WorldTools tools_;
  std::optional<core::ToolActivity> pending_{};
};

} // namespace pigpen::agent
