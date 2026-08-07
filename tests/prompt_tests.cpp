#include "agent/prompt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

[[nodiscard]] bool contains(const std::string &text,
                            const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("Agent configuration requires callers to select a model") {
  const pigpen::agent::Config config;

  CHECK(config.base_url == "http://127.0.0.1:11434/v1");
  CHECK(config.model.empty());
  CHECK(config.seed == 0);
  CHECK(config.turn_budget == 20);
  CHECK(config.max_tool_rounds == 8);
  CHECK(config.temperature == 0.0);
  CHECK(config.known_item_values);
  CHECK(config.reward_feedback);
  CHECK_FALSE(config.opaque_look);
}

TEST_CASE(
    "System prompt establishes embodiment tools autonomy and finite limits") {
  pigpen::agent::Config config;
  config.turn_budget = 11;
  config.max_tool_rounds = 6;
  const auto prompt = pigpen::agent::build_system_prompt(config);

  CHECK(contains(prompt, "autonomous blob"));
  CHECK(contains(prompt, "Act on your own immediately"));
  CHECK(contains(prompt, "do not wait for the human"));
  CHECK(contains(prompt, "south-west corner is (0,0)"));
  CHECK(contains(prompt, "x grows east and y grows north"));
  CHECK(contains(prompt, "begin at (5,5)"));
  CHECK(contains(prompt, "look(direction)"));
  CHECK(contains(prompt, "move(direction)"));
  CHECK(contains(prompt, "eat()"));
  CHECK(contains(prompt, "at most 11 conversation turns"));
  CHECK(contains(prompt, "at most 6 tool rounds per turn"));
  CHECK(contains(prompt, "between one and four useful tool calls"));
  CHECK(contains(prompt, "followed by a short final action summary"));
  CHECK(contains(prompt, "Use look and move calls proactively"));
  CHECK(contains(prompt, "move never collects or consumes an item"));
  CHECK(contains(prompt, "Only eat can consume one"));
  CHECK(contains(prompt, "call eat while still on that cell"));
  CHECK(contains(prompt, "Do not provide hidden chain-of-thought"));
}

TEST_CASE("Known-values prompt states the complete reward table") {
  pigpen::agent::Config config;
  config.known_item_values = true;
  const auto prompt = pigpen::agent::build_system_prompt(config);

  CHECK(contains(prompt, "berry = +1"));
  CHECK(contains(prompt, "apple = +3"));
  CHECK(contains(prompt, "truffle = +10"));
  CHECK(contains(prompt, "toadstool = -5"));
  CHECK(contains(prompt, "Avoid eating negative-value food"));
  CHECK(contains(prompt, "Walking across a toadstool cell is safe"));
  CHECK(contains(prompt, "avoid eating the toadstool, not traversing"));
}

TEST_CASE("Hidden-values prompt does not leak the reward table") {
  pigpen::agent::Config config;
  config.known_item_values = false;
  const auto prompt = pigpen::agent::build_system_prompt(config);

  CHECK(contains(prompt, "Item values are hidden"));
  CHECK(contains(prompt, "Different foods can have different values"));
  CHECK_FALSE(contains(prompt, "berry ="));
  CHECK_FALSE(contains(prompt, "apple ="));
  CHECK_FALSE(contains(prompt, "truffle ="));
  CHECK_FALSE(contains(prompt, "toadstool ="));
  CHECK_FALSE(contains(prompt, "+10"));
  CHECK_FALSE(contains(prompt, "-5"));
}

TEST_CASE("Prompt accurately describes observation and feedback toggles") {
  pigpen::agent::Config transparent;
  const auto default_prompt = pigpen::agent::build_system_prompt(transparent);
  CHECK(contains(default_prompt, "look identifies the item type"));
  CHECK(contains(default_prompt, "numeric reward and your cumulative score"));

  auto opaque = transparent;
  opaque.opaque_look = true;
  opaque.reward_feedback = false;
  const auto experimental_prompt = pigpen::agent::build_system_prompt(opaque);
  CHECK(contains(experimental_prompt, "as 'something'"));
  CHECK(contains(experimental_prompt, "without revealing its item type"));
  CHECK(contains(experimental_prompt, "withholds numeric reward"));
}

TEST_CASE("Turn prompts sustain exploration and carry optional human input") {
  const auto automatic = pigpen::agent::build_turn_prompt(7, 20);
  CHECK(contains(automatic, "Continue exploring autonomously"));
  CHECK(contains(automatic, "one to four world-tool calls"));
  CHECK(contains(automatic, "brief action summary"));
  CHECK(contains(automatic, "move never eats an item"));
  CHECK(contains(automatic, "call eat explicitly"));
  CHECK(contains(automatic, "Turn 7 of 20."));
  CHECK(contains(automatic, "Automatic turn instructions:"));

  const auto guided =
      pigpen::agent::build_turn_prompt(8, 20, "Please inspect the north wall.");
  CHECK(contains(guided, "Turn 8 of 20."));
  CHECK(contains(guided, "Human guidance:\nPlease inspect the north wall."));
  CHECK(contains(guided, "one to four world-tool calls"));

  const auto corrective = pigpen::agent::build_turn_prompt(9, 20, {}, true);
  CHECK(contains(corrective, "previous turn executed zero world tools"));
  CHECK(contains(corrective, "Model narration is not an action"));
  CHECK(contains(corrective, "Begin this turn with a valid"));
}
