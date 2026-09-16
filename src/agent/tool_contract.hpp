/// @file tool_contract.hpp
/// @brief Reflected argument and response types: the model-facing contract.
///
/// Scry derives closed JSON Schemas from these declarations at compile time
/// (P2996/P3394), strictly decodes incoming arguments, and encodes the typed
/// responses. Adding or renaming a member or enumerator changes schema,
/// decode, and encode from this one place.
#pragma once

#include "world/world.hpp"

#include <scry/reflection.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// @brief Shared reflected input for tools that act along a cardinal
/// direction.
struct DirectionArguments {
  // clang-format off: keep the P3394 annotation visually separate from its type.
  [[=scry::reflection::description{
      "Cardinal direction: north, south, east, or west"}]]
  world::Direction direction;
  // clang-format on
};

/// @brief Reflected input for a tool that accepts no arguments.
struct EatArguments {};

/// @brief World result for an admitted `move` call.
struct MoveToolResponse {
  bool ok{};
  std::optional<world::ItemType> item_here{};
  world::Position position{};
  std::optional<world::MoveFailure> reason{};
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
  world::Direction direction{};
  std::vector<LookToolCell> cells{};
  int wall_at_distance{};
};

/// @brief Response envelope for `eat`; `reward` and `score` are withheld in
/// no-reward-feedback episodes.
struct EatToolResponse {
  bool ok{};
  std::optional<world::ItemType> ate{};
  std::optional<world::EatFailure> reason{};
  std::optional<int> reward{};
  std::optional<int> score{};
};

static_assert(scry::reflection::ToolArguments<DirectionArguments>);
static_assert(scry::reflection::ToolArguments<EatArguments>);
static_assert(scry::reflection::SupportedValue<MoveToolResponse>);
static_assert(scry::reflection::SupportedValue<LookToolResponse>);
static_assert(scry::reflection::SupportedValue<EatToolResponse>);

} // namespace pigpen::agent
