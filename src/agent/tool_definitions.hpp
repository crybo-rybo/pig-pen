/// @file tool_definitions.hpp
/// @brief One enumeration of tool bindings for registration and build
/// artifacts.
#pragma once

#include "agent/events.hpp"
#include "agent/prompt_text.hpp"
#include "agent/world_tools.hpp"

namespace pigpen::agent {

/// Visit each binding with its argument type, kind, description, and handler.
/// Both the live registry and the manifest derive schemas from these types.
template <typename Visitor> void for_each_tool_definition(Visitor &&visitor) {
  visitor.template operator()<DirectionArguments>(
      ToolKind::move, prompt_text::tool_move, &WorldTools::move);
  visitor.template operator()<DirectionArguments>(
      ToolKind::look, prompt_text::tool_look, &WorldTools::look);
  visitor.template operator()<EatArguments>(
      ToolKind::eat, prompt_text::tool_eat, &WorldTools::eat);
}

} // namespace pigpen::agent
