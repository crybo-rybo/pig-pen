/// @file prompt.cpp
/// @brief Assembles embedded prompt text using the episode configuration.
#include "agent/prompt.hpp"

#include "agent/prompt_text.hpp"
#include "world/world.hpp"

#include <format>
#include <string>

namespace pigpen::agent {

std::string build_system_prompt(const Config &config) {
  std::string prompt{prompt_text::system_intro};
  prompt += std::format(prompt_text::system_coordinates, world::World::spawn.x,
                        world::World::spawn.y);
  prompt += prompt_text::system_tools;
  prompt += config.known_item_values ? prompt_text::known_item_values
                                     : prompt_text::hidden_item_values;
  prompt += config.opaque_look ? prompt_text::opaque_look
                               : prompt_text::transparent_look;
  prompt += config.reward_feedback ? prompt_text::reward_feedback
                                   : prompt_text::no_reward_feedback;
  prompt += std::format(prompt_text::system_limits, config.turn_budget,
                        config.max_tool_rounds, max_world_tool_calls_per_turn);
  return prompt;
}

std::string build_turn_prompt(const std::size_t turn,
                              const std::size_t turn_budget,
                              const std::string_view human_input,
                              const bool recover_zero_tool_turn) {
  auto prompt = std::format(prompt_text::turn_instructions,
                            max_world_tool_calls_per_turn, turn, turn_budget);
  if (recover_zero_tool_turn) {
    prompt += prompt_text::zero_tool_correction;
  }
  if (!human_input.empty()) {
    prompt += prompt_text::human_guidance;
    prompt.append(human_input);
  }
  return prompt;
}

} // namespace pigpen::agent
