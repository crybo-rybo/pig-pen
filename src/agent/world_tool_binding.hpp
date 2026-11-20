/// @file world_tool_binding.hpp
/// @brief The world tools as a scrydantic toolbox, and the dispatcher that
/// the tool loop calls.
///
/// Requires C++26 reflection: include it only from pigpen_agent and the
/// reflection tests. The C++23 layers make the dispatcher through
/// agent/world_tool_dispatcher.hpp.
#pragma once

#include "agent/tool_contract.hpp"
#include "agent/world_tool_dispatcher.hpp"
#include "core/config.hpp"
#include "core/events.hpp"
#include "core/tool_dispatcher.hpp"
#include "core/world_tools.hpp"

#include <scrydantic/scrydantic.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::agent {

/// @brief The call context of the world toolbox. scrydantic gives it to each
/// tool member as the leading `const WorldToolCall &` parameter; it is not
/// part of the schema.
///
/// The dispatcher makes one for each call, with a new empty output slot. A
/// member that runs its world action writes the transition into the slot.
/// The slot stays empty when the arguments do not decode, because then
/// scrydantic does not call the member.
struct WorldToolCall {
  /// The call identity from the tool loop.
  core::ToolCallContext call{};
  /// Where the member writes its world transition. It refers to storage of
  /// the dispatcher, so the const context can still fill it.
  std::optional<core::ToolActivity> &activity;
};

/// @brief scrydantic toolbox whose annotated members are the model's world
/// tools.
///
/// scrydantic names each tool after its member function and generates its
/// schema, argument decoder, and result encoder from the declaration. Each
/// member runs the typed world action and writes its ToolActivity, with the
/// canonical arguments, into the slot of its WorldToolCall;
/// WorldToolDispatcher then adds the exact result text and publishes it.
class WorldToolBinding final {
public:
  WorldToolBinding(world::World &world, const core::Config &config);
  ~WorldToolBinding();
  WorldToolBinding(const WorldToolBinding &) = delete;
  WorldToolBinding &operator=(const WorldToolBinding &) = delete;

  // clang-format off: keep each P3394 annotation on its own line.
  [[= scrydantic::tool{"Move one cell north, south, east, or west."}]]
  core::MoveToolResponse move(const WorldToolCall &context,
                              DirectionArguments arguments);
  [[= scrydantic::tool{"Scan every cell in one direction to the wall."}]]
  core::LookToolResponse look(const WorldToolCall &context,
                              DirectionArguments arguments);
  [[= scrydantic::tool{"Eat the item on the current cell, if present."}]]
  core::EatToolResponse eat(const WorldToolCall &context);
  // clang-format on

private:
  core::ToolActivity &record(const WorldToolCall &context, core::ToolKind kind,
                             core::ToolOutcome outcome, world::Position before);

  world::World &world_;
  core::WorldTools tools_;
};

/// @brief core::IToolDispatcher over the bound world tools: admission, find
/// by name, strict decode, the world action, and the encoded result.
///
/// The bound handlers share ownership of the binding; the world that it
/// references must outlive this dispatcher.
class WorldToolDispatcher final : public core::IToolDispatcher {
public:
  WorldToolDispatcher(world::World &world, const core::Config &config,
                      WorldToolObservers observers);
  ~WorldToolDispatcher() override;
  WorldToolDispatcher(const WorldToolDispatcher &) = delete;
  WorldToolDispatcher &operator=(const WorldToolDispatcher &) = delete;

  /// @brief move, look, and eat, in declaration order.
  [[nodiscard]] const std::vector<core::ChatTool> &tools() const override {
    return tools_;
  }
  [[nodiscard]] core::ToolDispatchResult
  dispatch(const core::ToolCallContext &context, std::string_view name,
           std::string_view arguments_json) override;

private:
  const world::World &world_;
  WorldToolObservers observers_;
  std::vector<scrydantic::BoundTool<WorldToolCall>> bound_;
  std::vector<core::ChatTool> tools_{};
  std::string registered_names_{};
};

} // namespace pigpen::agent
