/// @file tool_contract.hpp
/// @brief The reflected model-facing contract: annotated tool arguments, and
/// the checks that every argument and response type is one Scry can reflect.
///
/// Scry derives closed JSON Schemas from these declarations at compile time
/// (P2996/P3394), strictly decodes incoming arguments, and encodes the typed
/// responses declared in tool_responses.hpp. Adding or renaming a member or
/// enumerator changes schema, decode, and encode from its one declaration.
/// Requires C++26 reflection: include it only from pigpen_agent.
#pragma once

#include "agent/tool_responses.hpp"
#include "world/world.hpp"

#include <scry/reflection.hpp>

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

static_assert(scry::reflection::ToolArguments<DirectionArguments>);
static_assert(scry::reflection::SupportedValue<MoveToolResponse>);
static_assert(scry::reflection::SupportedValue<LookToolResponse>);
static_assert(scry::reflection::SupportedValue<EatToolResponse>);

} // namespace pigpen::agent
