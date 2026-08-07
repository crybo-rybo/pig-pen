#pragma once

#include "agent/config.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// Builds the stable embodiment and experiment instructions for one episode.
[[nodiscard]] std::string build_system_prompt(const Config &config);

/// Builds the bounded automatic nudge for an episode turn. Human input, when
/// present, is delivered as part of the nudge without weakening the autonomy
/// instruction.
[[nodiscard]] std::string build_turn_prompt(std::size_t turn,
                                            std::size_t turn_budget,
                                            std::string_view human_input = {});

} // namespace pigpen::agent
