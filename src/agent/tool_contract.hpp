/// @file tool_contract.hpp
/// @brief Reflected argument and response types: the model-facing contract.
///
/// Scry derives closed JSON Schemas from these declarations at compile time
/// (P2996/P3394), strictly decodes incoming arguments, and encodes the typed
/// responses. Adding or renaming a member or enumerator changes schema,
/// decode, and encode from this one place.
#pragma once

#include "agent/prompt_text.hpp"
#include "world/world.hpp"

#include <scry/reflection.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

namespace detail {
inline constexpr auto direction_description = [] consteval {
  char text[prompt_text::tool_direction.size() + 1]{};
  for (std::size_t i = 0; i < prompt_text::tool_direction.size(); ++i) {
    text[i] = prompt_text::tool_direction[i];
  }
  return scry::reflection::description{text};
}();
} // namespace detail

/// @brief Shared reflected input for tools that act along a cardinal
/// direction.
struct DirectionArguments {
  // clang-format off: keep the P3394 annotation visually separate from its type.
  [[=detail::direction_description]]
  world::Direction direction;
  // clang-format on
};

/// @brief Reflected input for a tool that accepts no arguments.
struct EatArguments {};

/// @brief Per-turn action budget reported inside every tool response so the
/// model always knows how many world actions remain.
struct TurnToolBudget {
  std::size_t used{};
  std::size_t remaining{};
  std::string instruction{};
};

/// @brief Application-level failure reported inside a decoded response.
/// @note Distinct from protocol failures (unknown tool, undecodable
/// arguments), which scry rejects before any handler runs.
enum class ToolFailureCode {
  tool_budget_exhausted,
};

/// @brief Response envelope for `move`; `action_executed` is false when the
/// per-turn budget rejected the action.
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

/// @brief One scanned cell as shown to the model; `item` may read
/// "something" in opaque-look episodes.
struct LookToolCell {
  int distance{};
  std::optional<std::string> item{};
};

/// @brief Response envelope for `look`.
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

/// @brief Response envelope for `eat`; `reward` and `score` are withheld in
/// no-reward-feedback episodes.
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
