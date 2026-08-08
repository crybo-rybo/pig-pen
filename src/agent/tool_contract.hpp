#pragma once

#include "world/world.hpp"

#include <cstddef>
#include <optional>
#include <scry/reflection.hpp>
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

struct ToolFailure {
  ToolFailureCode code{};
  std::string message{};
};

template <typename Result> struct ToolResponse {
  std::optional<ToolFailure> error{};
  std::optional<Result> result{};
  TurnToolBudget turn_tool_budget{};
};

struct MoveToolResult {
  std::optional<world::ItemType> item_here{};
  bool ok{};
  world::Position position{};
  std::optional<world::MoveFailure> reason{};
};

struct LookToolCell {
  int distance{};
  std::optional<std::string> item{};
};

struct LookToolResult {
  std::vector<LookToolCell> cells{};
  world::Direction direction{};
  int wall_at_distance{};
};

struct EatToolResult {
  std::optional<world::ItemType> ate{};
  bool ok{};
  std::optional<world::EatFailure> reason{};
  std::optional<int> reward{};
  std::optional<int> score{};
};

using MoveToolResponse = ToolResponse<MoveToolResult>;
using LookToolResponse = ToolResponse<LookToolResult>;
using EatToolResponse = ToolResponse<EatToolResult>;

static_assert(scry::reflection::ToolArguments<DirectionArguments>);
static_assert(scry::reflection::ToolArguments<EatArguments>);
static_assert(scry::reflection::SupportedValue<MoveToolResponse>);
static_assert(scry::reflection::SupportedValue<LookToolResponse>);
static_assert(scry::reflection::SupportedValue<EatToolResponse>);

} // namespace pigpen::agent
