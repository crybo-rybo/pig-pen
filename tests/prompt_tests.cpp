/// @file prompt_tests.cpp
/// @brief Covers Config defaults and that each prompt flag says what it
/// claims — including that the hidden-values prompt never leaks the reward
/// table.

#include "agent/prompt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Agent configuration requires callers to select a model") {
  const pigpen::agent::Config config;

  CHECK(config.base_url == "http://127.0.0.1:11434/v1");
  CHECK(config.model.empty());
  CHECK(config.seed == 0);
  CHECK(config.turn_budget == 20);
  CHECK(config.max_tool_rounds == 8);
  CHECK(config.max_output_tokens == 8'192);
  CHECK(pigpen::agent::max_world_tool_calls_per_turn == 4);
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

  CHECK(prompt.contains("autonomous blob"));
  CHECK(prompt.contains("Act on your own immediately"));
  CHECK(prompt.contains("do not wait for the human"));
  CHECK(prompt.contains("south-west corner is (0,0)"));
  CHECK(prompt.contains("x grows east and y grows north"));
  CHECK(prompt.contains("begin at (5,5)"));
  CHECK(prompt.contains("look(direction)"));
  CHECK(prompt.contains("move(direction)"));
  CHECK(prompt.contains("eat()"));
  CHECK(prompt.contains("at most 11 conversation turns"));
  CHECK(prompt.contains("at most 6 tool rounds per turn"));
  CHECK(prompt.contains("allows at most 4 world-tool requests"));
  CHECK(prompt.contains("including invalid requests"));
  CHECK(prompt.contains("followed by a short final action summary"));
  CHECK(prompt.contains("Prioritize calling the registered world tools"));
  CHECK(prompt.contains("over extended thinking"));
  CHECK(prompt.contains("Use look and move calls proactively"));
  CHECK(prompt.contains("move never collects or consumes an item"));
  CHECK(prompt.contains("Only eat can consume one"));
  CHECK(prompt.contains("call eat while still on that cell"));
  CHECK(prompt.contains("Do not provide hidden chain-of-thought"));
}

TEST_CASE("Known-values prompt states the complete reward table") {
  pigpen::agent::Config config;
  config.known_item_values = true;
  const auto prompt = pigpen::agent::build_system_prompt(config);

  CHECK(prompt.contains("berry = +1"));
  CHECK(prompt.contains("apple = +3"));
  CHECK(prompt.contains("truffle = +10"));
  CHECK(prompt.contains("toadstool = -5"));
  CHECK(prompt.contains("Avoid eating negative-value food"));
  CHECK(prompt.contains("Walking across a toadstool cell is safe"));
  CHECK(prompt.contains("avoid eating the toadstool, not traversing"));
}

TEST_CASE("Hidden-values prompt does not leak the reward table") {
  pigpen::agent::Config config;
  config.known_item_values = false;
  const auto prompt = pigpen::agent::build_system_prompt(config);

  CHECK(prompt.contains("Item values are hidden"));
  CHECK(prompt.contains("Different foods can have different values"));
  CHECK_FALSE(prompt.contains("berry ="));
  CHECK_FALSE(prompt.contains("apple ="));
  CHECK_FALSE(prompt.contains("truffle ="));
  CHECK_FALSE(prompt.contains("toadstool ="));
  CHECK_FALSE(prompt.contains("+10"));
  CHECK_FALSE(prompt.contains("-5"));
}

TEST_CASE("Prompt accurately describes observation and feedback toggles") {
  pigpen::agent::Config transparent;
  const auto default_prompt = pigpen::agent::build_system_prompt(transparent);
  CHECK(default_prompt.contains("look identifies the item type"));
  CHECK(default_prompt.contains("numeric reward and your cumulative score"));

  auto opaque = transparent;
  opaque.opaque_look = true;
  opaque.reward_feedback = false;
  const auto experimental_prompt = pigpen::agent::build_system_prompt(opaque);
  CHECK(experimental_prompt.contains("as 'something'"));
  CHECK(experimental_prompt.contains("without revealing its item type"));
  CHECK(experimental_prompt.contains("withholds numeric reward"));
}

TEST_CASE("Turn prompts sustain exploration and carry optional human input") {
  const auto automatic = pigpen::agent::build_turn_prompt(7, 20);
  CHECK(automatic.contains("Continue exploring autonomously"));
  CHECK(automatic.contains("up to 4 world-tool calls"));
  CHECK(automatic.contains("brief action summary"));
  CHECK(automatic.contains("move never eats an item"));
  CHECK(automatic.contains("call eat explicitly"));
  CHECK(automatic.contains("Turn 7 of 20."));
  CHECK(automatic.contains("Automatic turn instructions:"));

  const auto guided =
      pigpen::agent::build_turn_prompt(8, 20, "Please inspect the north wall.");
  CHECK(guided.contains("Turn 8 of 20."));
  CHECK(guided.contains("Human guidance:\nPlease inspect the north wall."));
  CHECK(guided.contains("up to 4 world-tool calls"));

  const auto corrective = pigpen::agent::build_turn_prompt(9, 20, {}, true);
  CHECK(corrective.contains("previous turn executed zero world tools"));
  CHECK(corrective.contains("Model narration is not an action"));
  CHECK(corrective.contains("Begin this turn with a valid"));
}

TEST_CASE("turn recovery instructions remain separate from human guidance") {
  const auto prompt = pigpen::agent::build_turn_prompt(
      2, 20, "Please inspect the north wall.", true, 3);
  const auto notice = prompt.find("3 requested tool calls were not executed");
  REQUIRE(notice != std::string::npos);
  CHECK(notice < prompt.find("Human guidance:"));
  CHECK(prompt.ends_with("Human guidance:\nPlease inspect the north wall."));
  CHECK_FALSE(pigpen::agent::build_turn_prompt(3, 20).contains(
      "requested tool calls were not executed"));
}
