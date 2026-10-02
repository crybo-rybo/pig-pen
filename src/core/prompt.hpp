/// @file prompt.hpp
/// @brief Builders for the system prompt and the per-turn automatic nudge.
///
/// System instructions derive from Config; per-turn nudges also use recorded
/// turn feedback and separately labelled human guidance.
#pragma once

#include "core/config.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace pigpen::core {

/// @brief Builds the stable embodiment and experiment instructions for one
/// episode.
/// @note The Config visibility flags decide what the prompt reveals; with
/// `known_item_values` off the reward table must never appear.
[[nodiscard]] std::string build_system_prompt(const Config &config);

/// @brief Builds the bounded automatic nudge for an episode turn.
///
/// Human guidance, when present, is delivered as a separately labelled
/// section without weakening the autonomy instruction.
/// @param recover_zero_tool_turn Appends a correction requiring a decoded
/// world-tool invocation after a turn that produced none.
/// @param unexecuted_tool_calls Reports calls dropped at the previous round
/// cap.
[[nodiscard]] std::string
build_turn_prompt(std::size_t turn, std::size_t turn_budget,
                  std::string_view human_input = {},
                  bool recover_zero_tool_turn = false,
                  std::size_t unexecuted_tool_calls = 0);

} // namespace pigpen::core
