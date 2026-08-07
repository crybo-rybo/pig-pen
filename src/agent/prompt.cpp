#include "agent/prompt.hpp"

#include "world/world.hpp"

#include <array>
#include <string>

namespace pigpen::agent {
namespace {

constexpr std::array<std::string_view, 4> fallback_look_directions{
    "north", "east", "south", "west"};

void append_experiment_instructions(std::string &prompt, const Config &config) {
  if (config.known_item_values) {
    prompt += "Item values are known: berry = +1, apple = +3, truffle = +10, "
              "and toadstool = -5. Avoid eating negative-value food. Walking "
              "across a toadstool cell is safe because movement does not eat "
              "it; avoid eating the toadstool, not traversing its cell.\n";
  } else {
    prompt +=
        "Item values are hidden. Different foods can have different values; "
        "explore and use only the feedback actually returned by tools to "
        "decide "
        "what is worthwhile.\n";
  }

  if (config.opaque_look) {
    prompt +=
        "In this episode look reports an occupied cell as 'something' without "
        "revealing its item type.\n";
  } else {
    prompt +=
        "In this episode look identifies the item type in an occupied cell.\n";
  }

  if (config.reward_feedback) {
    prompt += "A successful eat reports the item's numeric reward and your "
              "cumulative "
              "score.\n";
  } else {
    prompt += "A successful eat reports what was eaten but withholds numeric "
              "reward and "
              "cumulative score.\n";
  }
}

} // namespace

std::string build_system_prompt(const Config &config) {
  std::string prompt;
  prompt.reserve(1'600);
  prompt +=
      "You are an autonomous blob embodied in a walled 10 by 10 pen. Your goal "
      "is to explore and maximize your score by finding and eating valuable "
      "items. Act on your own immediately and keep exploring; do not wait for "
      "the "
      "human to tell you where to move.\n";
  prompt += "Coordinates run from 0 through 9. The south-west corner is (0,0); "
            "x grows "
            "east and y grows north. You begin at (" +
            std::to_string(world::World::spawn.x) + "," +
            std::to_string(world::World::spawn.y) +
            "). The human can see the whole "
            "pen, but you cannot. Never claim to see cells that a tool has not "
            "revealed.\n";
  prompt +=
      "Use only these world tools: look(direction) scans north, south, east, "
      "or "
      "west to the wall; move(direction) moves exactly one cell; eat() "
      "consumes "
      "the item at your current cell. A wall-blocked move and eating an empty "
      "cell are recoverable outcomes, so adjust and continue. Use look and "
      "move "
      "calls proactively rather than merely describing what you might do. "
      "Critical mechanic: move never collects or consumes an item and never "
      "changes your score. Only eat can consume one. When move reports an "
      "item_here that you intend to collect, call eat while still on that "
      "cell.\n";
  append_experiment_instructions(prompt, config);
  prompt +=
      "You have at most " + std::to_string(config.turn_budget) +
      " conversation turns, with at most " +
      std::to_string(config.max_tool_rounds) +
      " tool rounds per turn. Fast action protocol: decide only the next "
      "action, not the whole turn. When an action is possible, respond with "
      "exactly one useful tool call and no narration before it. After each "
      "tool result, either make the next obvious tool call the same way or "
      "finish. Never compare alternatives, restate these rules, explain a "
      "plan, or simulate future tool results. Choose the first valid action, "
      "not the best action. The automatic turn instruction supplies a "
      "concrete fallback look; use it immediately when no action is obvious. "
      "Each conversation turn must execute between one and four useful tool "
      "calls, then end with an action summary of at most eight words and no "
      "more tool calls. Another Continue message will arrive, so do not try "
      "to finish the whole episode at once. Keep any internal deliberation to "
      "one short sentence. Do not provide hidden chain-of-thought. Stop when "
      "all positive-value items are gone or the episode ends.\n";
  return prompt;
}

std::string build_turn_prompt(const std::size_t turn,
                              const std::size_t turn_budget,
                              const std::string_view human_input,
                              const bool recover_zero_tool_turn) {
  const auto fallback_index =
      turn == 0 ? 0U : (turn - 1U) % fallback_look_directions.size();
  const auto fallback_direction = fallback_look_directions[fallback_index];
  std::string prompt =
      "Automatic turn instructions:\n"
      "ACTION ONLY. Choose the first valid action, not the best action. Make "
      "exactly one world-tool call now with no preamble or analysis. If no "
      "action is obvious, call look(";
  prompt.append(fallback_direction);
  prompt +=
      "). After its result, take one obvious follow-up directly or finish. "
      "Use one to four world-tool calls total, then summarize in at most eight "
      "words. Remember: move never eats an item; call eat explicitly on an "
      "item_here. Turn " +
      std::to_string(turn) + " of " + std::to_string(turn_budget) + ".";
  if (recover_zero_tool_turn) {
    prompt +=
        "\nCorrection: the previous turn executed zero world tools. Model "
        "narration is not an action. Begin this turn with a valid look, move, "
        "or eat tool call before summarizing.";
  }
  if (!human_input.empty()) {
    prompt += "\n\nHuman guidance:\n";
    prompt.append(human_input);
  }
  return prompt;
}

} // namespace pigpen::agent
