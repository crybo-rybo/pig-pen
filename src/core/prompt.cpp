/// @file prompt.cpp
/// @brief Assembles the system prompt and per-turn nudge text from a Config.
#include "core/prompt.hpp"

#include "world/world.hpp"

#include <format>
#include <string>

namespace pigpen::core {
namespace {

constexpr std::string_view embodiment =
    "You are an autonomous blob embodied in a walled 10 by 10 pen. Your goal "
    "is to explore and maximize your score by finding and eating valuable "
    "items. Act on your own immediately and keep exploring; do not wait for "
    "the human to tell you where to move.";

constexpr std::string_view coordinates =
    "Coordinates run from 0 through 9. The south-west corner is (0,0); x grows "
    "east and y grows north. You begin at ({},{}). The human can see the "
    "whole pen, but you cannot. Never claim to see cells that a tool has not "
    "revealed.";

constexpr std::string_view world_tools =
    "Use only these world tools: look(direction) scans north, south, east, or "
    "west to the wall; move(direction) moves exactly one cell; eat() consumes "
    "the item at your current cell. A wall-blocked move and eating an empty "
    "cell are recoverable outcomes, so adjust and continue. Use look and move "
    "calls proactively rather than merely describing what you might do. "
    "Critical mechanic: move never collects or consumes an item and never "
    "changes your score. Only eat can consume one. When move reports an "
    "item_here that you intend to collect, call eat while still on that "
    "cell.";

// Each Config visibility flag has exactly one honest sentence per state,
// which prompt_tests.cpp asserts on.
constexpr std::string_view known_values =
    "Item values are known: berry = +1, apple = +3, truffle = +10, and "
    "toadstool = -5. Avoid eating negative-value food. Walking across a "
    "toadstool cell is safe because movement does not eat it; avoid eating "
    "the toadstool, not traversing its cell.";
constexpr std::string_view hidden_values =
    "Item values are hidden. Different foods can have different values; "
    "explore and use only the feedback actually returned by tools to decide "
    "what is worthwhile.";
constexpr std::string_view opaque_look =
    "In this episode look reports an occupied cell as 'something' without "
    "revealing its item type.";
constexpr std::string_view transparent_look =
    "In this episode look identifies the item type in an occupied cell.";
constexpr std::string_view reward_feedback =
    "A successful eat reports the item's numeric reward and your cumulative "
    "score.";
constexpr std::string_view withheld_feedback =
    "A successful eat reports what was eaten but withholds numeric reward and "
    "cumulative score.";

constexpr std::string_view limits =
    "You have at most {} conversation turns, with at most {} tool rounds per "
    "turn. Pig Pen allows at most {} world-tool requests per conversation "
    "turn, including invalid requests. Excess calls are refused without "
    "acting. Each conversation turn should contain one or more useful tool "
    "calls, followed by a short final action summary with no more tool calls. "
    "Prioritize calling the registered world tools over extended thinking or "
    "describing what you might do. Do not try to finish the whole episode or "
    "consume every allowed tool round at once; another Continue message will "
    "arrive. Do not provide hidden chain-of-thought. Stop when all "
    "positive-value items are gone or the episode ends.";

constexpr std::string_view turn_instructions =
    "Automatic turn instructions:\n"
    "Continue exploring autonomously. Use up to {} world-tool calls now, then "
    "finish this turn with a brief action summary and no further tool call. "
    "Remember: move never eats an item; call eat explicitly to consume an "
    "item_here. Turn {} of {}.";

constexpr std::string_view zero_tool_correction =
    "\nCorrection: the previous turn executed zero world tools. Model "
    "narration is not an action. Begin this turn with a valid look, move, or "
    "eat tool call before summarizing.";

constexpr std::string_view round_limit_notice =
    "\nThe previous turn reached its tool-round limit. {} requested tool calls "
    "were not executed. Choose your next action from the tool results "
    "actually received.";

} // namespace

std::string build_system_prompt(const Config &config) {
  return std::format(
      "{}\n{}\n{}\n{}\n{}\n{}\n{}\n", embodiment,
      std::format(coordinates, world::World::spawn.x, world::World::spawn.y),
      world_tools, config.known_item_values ? known_values : hidden_values,
      config.opaque_look ? opaque_look : transparent_look,
      config.reward_feedback ? reward_feedback : withheld_feedback,
      std::format(limits, config.turn_budget, config.max_tool_rounds,
                  max_world_tool_calls_per_turn));
}

std::string build_turn_prompt(const std::size_t turn,
                              const std::size_t turn_budget,
                              const std::string_view human_input,
                              const bool recover_zero_tool_turn,
                              const std::size_t unexecuted_tool_calls) {
  auto prompt = std::format(turn_instructions, max_world_tool_calls_per_turn,
                            turn, turn_budget);
  if (recover_zero_tool_turn) {
    prompt += zero_tool_correction;
  }
  if (unexecuted_tool_calls != 0U) {
    prompt += std::format(round_limit_notice, unexecuted_tool_calls);
  }
  if (!human_input.empty()) {
    prompt += "\n\nHuman guidance:\n";
    prompt += human_input;
  }
  return prompt;
}

} // namespace pigpen::core
