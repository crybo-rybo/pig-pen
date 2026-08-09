#pragma once

#include "world/world.hpp"

#include <scry/reflection.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// Shared reflected input for tools that act along a cardinal direction.
struct DirectionArguments {
  // clang-format off: keep the P3394 annotation visually separate from its type.
  [[=scry::reflection::description{
      "Cardinal direction: north, south, east, or west"}]]
  world::Direction direction;
  // clang-format on
};

/// Reflected input for a tool that accepts no arguments.
struct EatArguments {};

struct TurnToolBudget {
  std::size_t used{};
  std::size_t remaining{};
  std::string instruction{};
};

enum class ToolFailureCode {
  tool_budget_exhausted,
};

struct MoveToolResponse {
  bool ok{};
  bool action_executed{};
  std::optional<ToolFailureCode> error_code{};
  std::optional<std::string> error{};
  std::optional<world::ItemType> item_here{};
  world::Position position{};
  std::optional<world::MoveFailure> reason{};
  TurnToolBudget turn_tool_budget{};
};

struct LookToolCell {
  int distance{};
  std::optional<std::string> item{};
};

struct LookToolResponse {
  bool ok{};
  bool action_executed{};
  std::optional<ToolFailureCode> error_code{};
  std::optional<std::string> error{};
  world::Direction direction{};
  std::vector<LookToolCell> cells{};
  int wall_at_distance{};
  TurnToolBudget turn_tool_budget{};
};

struct EatToolResponse {
  bool ok{};
  bool action_executed{};
  std::optional<ToolFailureCode> error_code{};
  std::optional<std::string> error{};
  std::optional<world::ItemType> ate{};
  std::optional<world::EatFailure> reason{};
  std::optional<int> reward{};
  std::optional<int> score{};
  TurnToolBudget turn_tool_budget{};
};

static_assert(scry::reflection::ToolArguments<DirectionArguments>);
static_assert(scry::reflection::ToolArguments<EatArguments>);
static_assert(scry::reflection::SupportedValue<MoveToolResponse>);
static_assert(scry::reflection::SupportedValue<LookToolResponse>);
static_assert(scry::reflection::SupportedValue<EatToolResponse>);

} // namespace pigpen::agent
