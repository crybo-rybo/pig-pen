/// @file world_tool_dispatcher.hpp
/// @brief The factory of the world tool dispatcher, for the C++23 layers.
///
/// Plain C++23: this header includes no scrydantic header and needs no
/// reflection, so any target that links pigpen_agent can include it. The
/// toolbox and the dispatcher class are in agent/world_tool_binding.hpp,
/// which needs C++26 reflection.
#pragma once

#include "core/config.hpp"
#include "core/events.hpp"
#include "core/tool_dispatcher.hpp"
#include "world/world.hpp"

#include <functional>
#include <memory>

namespace pigpen::agent {

/// @brief What the world tool dispatcher reports to its owner. Both are
/// called synchronously inside IToolDispatcher::dispatch(), on the pump
/// thread. An empty function is not called.
struct WorldToolObservers {
  /// Called one time for each decoded, admitted world action, after the
  /// action ran, with its exact canonical arguments and result text.
  std::function<void(core::ToolActivity)> on_activity{};
  /// Polled before each call; true refuses the call.
  std::function<bool()> logging_failed{};
};

/// @brief Make the core::IToolDispatcher of the world tools: admission
/// (core::admission_refusal), find by name, strict decode, the world action,
/// and the encoded result.
///
/// The observers are fixed here; nothing can change them later.
/// @param world The world that the tools change. It must outlive the
/// returned dispatcher: the dispatcher keeps a reference to it.
/// @param config Read here only, for the scenario visibility flags.
/// @param observers The callbacks of the owner.
[[nodiscard]] std::unique_ptr<core::IToolDispatcher>
make_world_tool_dispatcher(world::World &world, const core::Config &config,
                           WorldToolObservers observers);

} // namespace pigpen::agent
