/// @file world_tool_binding.hpp
/// @brief The world tools as a scrydantic toolbox, and the dispatcher that
/// the tool loop calls.
///
/// Requires C++26 reflection: include it only from pigpen_agent and the
/// reflection tests.
#pragma once

#include "agent/tool_contract.hpp"
#include "core/config.hpp"
#include "core/events.hpp"
#include "core/tool_dispatcher.hpp"
#include "core/world_tools.hpp"

#include <scrydantic/scrydantic.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::agent {

/// @brief scrydantic toolbox whose annotated members are the model's world
/// tools.
///
/// scrydantic names each tool after its member function and generates its
/// schema, argument decoder, and result encoder from the declaration. Each
/// member runs the typed world action and stages its ToolActivity, with the
/// canonical arguments; WorldToolDispatcher then adds the exact result text
/// and publishes it.
class WorldToolBinding final {
public:
  WorldToolBinding(world::World &world, const core::Config &config);
  ~WorldToolBinding();
  WorldToolBinding(const WorldToolBinding &) = delete;
  WorldToolBinding &operator=(const WorldToolBinding &) = delete;

  // clang-format off: keep each P3394 annotation on its own line.
  [[= scrydantic::tool{"Move one cell north, south, east, or west."}]]
  core::MoveToolResponse move(const core::ToolCallContext &context,
                              DirectionArguments arguments);
  [[= scrydantic::tool{"Scan every cell in one direction to the wall."}]]
  core::LookToolResponse look(const core::ToolCallContext &context,
                              DirectionArguments arguments);
  [[= scrydantic::tool{"Eat the item on the current cell, if present."}]]
  core::EatToolResponse eat(const core::ToolCallContext &context);
  // clang-format on

  /// @brief The refusal text for the model when world actions must stop
  /// (a log failure, or the objective is complete), else nothing.
  [[nodiscard]] std::optional<std::string_view>
  admission_refusal(bool logging_failed) const;
  /// @brief The transition that the last member call staged, once.
  [[nodiscard]] std::optional<core::ToolActivity> take_staged();

private:
  core::ToolActivity &stage(const core::ToolCallContext &context,
                            core::ToolKind kind, core::ToolOutcome outcome,
                            world::Position before);

  world::World &world_;
  core::WorldTools tools_;
  std::optional<core::ToolActivity> staged_{};
};

/// @brief core::IToolDispatcher over the bound world tools: admission, find
/// by name, strict decode, the world action, and the encoded result.
///
/// The bound handlers share ownership of the binding; the world that it
/// references must outlive this dispatcher.
class WorldToolDispatcher final : public core::IToolDispatcher {
public:
  /// @param logging_failed Polled before each call; true refuses it.
  WorldToolDispatcher(std::shared_ptr<WorldToolBinding> binding,
                      std::function<bool()> logging_failed = {});
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

  /// Called only for decoded, admitted world actions, synchronously inside
  /// dispatch().
  std::function<void(core::ToolActivity)> on_activity{};

private:
  std::shared_ptr<WorldToolBinding> binding_;
  std::function<bool()> logging_failed_;
  std::vector<scrydantic::BoundTool<core::ToolCallContext>> bound_;
  std::vector<core::ChatTool> tools_{};
  std::string registered_names_{};
};

} // namespace pigpen::agent
