/// @file world_tool_binding.hpp
/// @brief Bind typed world actions to Scry's registry and dispatch
/// observations.
#pragma once

#include "agent/events.hpp"
#include "agent/world_tools.hpp"

#include <scry/scry.hpp>

#include <functional>
#include <optional>

namespace pigpen::agent {

/// @brief Joins typed world transitions to the exact payload Scry dispatched.
/// @note Must outlive the single harness adopting registry(). Dispatch and
/// observation are serial on the pump thread, so only one transition can be
/// pending.
class WorldToolBinding final {
public:
  WorldToolBinding(world::World &world, const Config &config);
  WorldToolBinding(const WorldToolBinding &) = delete;
  WorldToolBinding &operator=(const WorldToolBinding &) = delete;

  [[nodiscard]] scry::Result<scry::ToolRegistry> registry();
  void observe(const scry::ToolCall &call);
  /// Retain a world side effect if Scry terminated before posting its result.
  void finish_turn();

  /// Called only for decoded, admitted world actions.
  std::function<void(ToolActivity)> on_activity{};

private:
  template <typename Arguments, typename Invoke>
  scry::Status add(scry::ToolRegistry &registry, ToolKind kind,
                   std::string description, Invoke invoke);

  world::World &world_;
  WorldTools tools_;
  std::optional<ToolActivity> pending_{};
};

} // namespace pigpen::agent
