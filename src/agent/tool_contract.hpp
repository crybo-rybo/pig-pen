/// @file tool_contract.hpp
/// @brief The reflected model-facing contract: annotated tool arguments, and
/// the checks that every argument and response type is one scrydantic can
/// reflect.
///
/// scrydantic derives closed JSON Schemas from these declarations at compile
/// time (P2996/P3394), strictly decodes incoming arguments, and encodes the
/// typed responses declared in tool_responses.hpp. Adding or renaming a member
/// or enumerator changes schema, decode, and encode from its one declaration.
/// Requires C++26 reflection: include it only from pigpen_agent.
#pragma once

#include "core/tool_responses.hpp"
#include "world/world.hpp"

#include <scrydantic/scrydantic.hpp>

#include <string>

namespace pigpen::agent {

/// @brief Shared reflected input for tools that act along a cardinal
/// direction.
struct DirectionArguments {
  // clang-format off: keep the P3394 annotation visually separate from its type.
  [[= scrydantic::description{
      "Cardinal direction: north, south, east, or west"}]]
  world::Direction direction;
  // clang-format on
};

/// @brief The tool result for a call that did not run: a decode error, an
/// unknown tool, or an admission refusal.
struct ToolErrorResponse {
  std::string error{};
};

static_assert(scrydantic::ToolArguments<DirectionArguments>);
static_assert(scrydantic::SupportedValue<core::MoveToolResponse>);
static_assert(scrydantic::SupportedValue<core::LookToolResponse>);
static_assert(scrydantic::SupportedValue<core::EatToolResponse>);
static_assert(scrydantic::SupportedValue<ToolErrorResponse>);

} // namespace pigpen::agent
