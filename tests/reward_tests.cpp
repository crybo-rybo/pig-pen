/// @file reward_tests.cpp
/// @brief Covers the shaped reward on plain facts: the default weights, each
/// term in isolation, the total as the sum of terms, validity by finish
/// reason, and `NAME=VALUE` weight parsing with each error.

#include "agent/reward.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace {

using pigpen::agent::compute_reward;
using pigpen::agent::EpisodeFacts;
using pigpen::agent::FinishReason;
using pigpen::agent::parse_reward_weight;
using pigpen::agent::RewardBreakdown;
using pigpen::agent::RewardWeights;

/// @brief A finished episode that contributes nothing to any term: no
/// score, only the spawn observed, and no turns.
[[nodiscard]] EpisodeFacts neutral_facts() {
  return {
      .finish_reason = FinishReason::turn_budget,
      .observed_cells = 1,
      .turn_budget = 20,
  };
}

/// @brief Every term except @p name contributes exactly +0.0.
void check_only_term(const RewardBreakdown &reward, const std::string &name,
                     const double expected) {
  REQUIRE(reward.terms.size() == pigpen::agent::reward_weight_fields.size());
  for (const auto &[term, contribution] : reward.terms) {
    if (term == name) {
      CHECK(contribution == Catch::Approx(expected));
    } else {
      CHECK(contribution == 0.0);
      CHECK_FALSE(std::signbit(contribution));
    }
  }
  CHECK(reward.total == Catch::Approx(expected));
}

} // namespace

TEST_CASE("reward weights default to the documented values", "[reward]") {
  const RewardWeights weights;
  CHECK(weights.score == 1.0);
  CHECK(weights.explored_cell == 0.05);
  CHECK(weights.active_turn == 0.1);
  CHECK(weights.zero_tool_turn == -1.0);
  CHECK(weights.failed_action == -0.1);
  CHECK(weights.invalid_call == -0.5);
  CHECK(weights.budget_refused_call == -0.25);
  CHECK(weights.objective == 5.0);
  CHECK(weights.unused_turn == 0.1);
  CHECK(pigpen::agent::reward_version == 1U);
}

TEST_CASE("a neutral finished episode earns exactly zero", "[reward]") {
  const auto reward = compute_reward(neutral_facts(), {});
  CHECK(reward.valid);
  CHECK(reward.invalid_reason.empty());
  CHECK(reward.total == 0.0);
  CHECK_FALSE(std::signbit(reward.total));
  CHECK(reward.terms.size() == 9);
  for (const auto &[term, contribution] : reward.terms) {
    CHECK(contribution == 0.0);
    // Zero counts times negative weights must not print as -0.0.
    CHECK_FALSE(std::signbit(contribution));
  }
}

TEST_CASE("each reward term counts in isolation", "[reward]") {
  const RewardWeights weights;
  auto facts = neutral_facts();

  SECTION("score, including a negative score") {
    facts.score = 14;
    auto reward = compute_reward(facts, weights);
    CHECK(reward.score == 14);
    check_only_term(reward, "score", 14.0);
    facts.score = -5;
    reward = compute_reward(facts, weights);
    check_only_term(reward, "score", -5.0);
  }
  SECTION("cells observed beyond the spawn") {
    facts.observed_cells = 58;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.explored_cells == 57);
    check_only_term(reward, "explored_cell", 57 * 0.05);
  }
  SECTION("zero observed cells never underflows") {
    facts.observed_cells = 0;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.explored_cells == 0);
    CHECK(reward.total == 0.0);
  }
  SECTION("active turns") {
    facts.active_turns = 19;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.active_turns == 19);
    check_only_term(reward, "active_turn", 1.9);
  }
  SECTION("zero-tool turns") {
    facts.zero_tool_turns = 2;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.zero_tool_turns == 2);
    check_only_term(reward, "zero_tool_turn", -2.0);
  }
  SECTION("failed actions") {
    facts.failed_actions = 3;
    facts.executed_actions = 10;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.failed_actions == 3);
    check_only_term(reward, "failed_action", -0.3);
  }
  SECTION("invalid calls") {
    facts.invalid_calls = 2;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.invalid_calls == 2);
    check_only_term(reward, "invalid_call", -1.0);
  }
  SECTION("budget-refused calls") {
    facts.budget_refused_calls = 3;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.budget_refused_calls == 3);
    check_only_term(reward, "budget_refused_call", -0.75);
  }
  SECTION("host-refused calls are not penalised") {
    facts.host_refused_calls = 4;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.total == 0.0);
  }
  SECTION("executed actions alone earn nothing") {
    facts.executed_actions = 12;
    CHECK(compute_reward(facts, weights).total == 0.0);
  }
  SECTION("unused turns without the objective earn nothing") {
    facts.turns_used = 5;
    const auto reward = compute_reward(facts, weights);
    CHECK(reward.unused_turns == 0);
    CHECK_FALSE(reward.objective_complete);
    CHECK(reward.total == 0.0);
  }
}

TEST_CASE("objective completion pays once plus every unused turn", "[reward]") {
  auto facts = neutral_facts();
  facts.finish_reason = FinishReason::objective_complete;
  facts.turns_used = 12;
  const auto reward = compute_reward(facts, {});
  CHECK(reward.valid);
  CHECK(reward.objective_complete);
  CHECK(reward.unused_turns == 8);
  CHECK(reward.terms.at("objective") == 5.0);
  CHECK(reward.terms.at("unused_turn") == Catch::Approx(0.8));
  CHECK(reward.total == Catch::Approx(5.8));

  // Completing on the last turn leaves nothing unused.
  facts.turns_used = 20;
  const auto last_turn = compute_reward(facts, {});
  CHECK(last_turn.unused_turns == 0);
  CHECK(last_turn.total == 5.0);
}

TEST_CASE("the reward total is the sum of every term", "[reward]") {
  const EpisodeFacts facts{
      .finish_reason = FinishReason::turn_budget,
      .score = 14,
      .observed_cells = 58,
      .turns_used = 20,
      .turn_budget = 20,
      .executed_actions = 68,
      .failed_actions = 3,
      .active_turns = 19,
      .zero_tool_turns = 1,
      .invalid_calls = 2,
      .budget_refused_calls = 1,
  };
  const auto reward = compute_reward(facts, {});
  double sum = 0.0;
  for (const auto &[term, contribution] : reward.terms) {
    sum += contribution;
  }
  CHECK(reward.total == Catch::Approx(sum));
  // 14 + 2.85 + 1.9 - 1 - 0.3 - 1 - 0.25
  CHECK(reward.total == Catch::Approx(16.2));

  // Custom weights scale their own term only.
  RewardWeights doubled_score;
  doubled_score.score = 2.0;
  CHECK(compute_reward(facts, doubled_score).total == Catch::Approx(30.2));
}

TEST_CASE("term counts line up with the weight table", "[reward]") {
  auto facts = neutral_facts();
  facts.finish_reason = FinishReason::objective_complete;
  facts.score = 7;
  facts.observed_cells = 4;
  facts.active_turns = 5;
  facts.zero_tool_turns = 1;
  facts.failed_actions = 2;
  facts.invalid_calls = 6;
  facts.budget_refused_calls = 8;
  facts.turns_used = 11;
  const auto reward = compute_reward(facts, {});
  const auto counts = pigpen::agent::reward_term_counts(reward);
  CHECK(counts == std::array{7.0, 3.0, 5.0, 1.0, 2.0, 6.0, 8.0, 1.0, 9.0});
  const RewardWeights weights;
  for (std::size_t index = 0; index < counts.size(); ++index) {
    const auto &field = pigpen::agent::reward_weight_fields[index];
    CHECK(reward.terms.at(std::string{field.name}) ==
          Catch::Approx(counts[index] * weights.*field.member));
  }
}

TEST_CASE("only turn_budget and objective_complete rewards are valid",
          "[reward]") {
  auto facts = neutral_facts();
  facts.score = 3;
  facts.invalid_calls = 1;

  for (const auto reason :
       {FinishReason::turn_budget, FinishReason::objective_complete}) {
    facts.finish_reason = reason;
    const auto reward = compute_reward(facts, {});
    CHECK(reward.valid);
    CHECK(reward.invalid_reason.empty());
  }

  const auto check_invalid = [&facts](const std::string &reason) {
    const auto reward = compute_reward(facts, {});
    CHECK_FALSE(reward.valid);
    CHECK(reward.invalid_reason == reason);
    // Counts and terms are still reported for offline analysis.
    CHECK(reward.score == 3);
    CHECK(reward.invalid_calls == 1);
    CHECK(reward.terms.at("score") == 3.0);
    CHECK(reward.total == Catch::Approx(2.5));
  };
  facts.finish_reason = FinishReason::stopped;
  check_invalid("stopped");
  facts.finish_reason = FinishReason::cancelled;
  check_invalid("cancelled");
  facts.finish_reason = FinishReason::error;
  check_invalid("error");
  facts.finish_reason.reset();
  check_invalid("unfinished");
}

TEST_CASE("reward weights parse from NAME=VALUE", "[reward]") {
  RewardWeights weights;
  REQUIRE(parse_reward_weight("invalid_call=-1.0", weights));
  CHECK(weights.invalid_call == -1.0);
  REQUIRE(parse_reward_weight("score=2", weights));
  CHECK(weights.score == 2.0);
  REQUIRE(parse_reward_weight("explored_cell=0", weights));
  CHECK(weights.explored_cell == 0.0);
  REQUIRE(parse_reward_weight("unused_turn=1e-2", weights));
  CHECK(weights.unused_turn == 0.01);
  REQUIRE(parse_reward_weight("failed_action=-0", weights));
  CHECK_FALSE(std::signbit(weights.failed_action));
  REQUIRE(parse_reward_weight("objective=1e6", weights));
  CHECK(weights.objective == 1e6);
  REQUIRE(parse_reward_weight("objective=-1e6", weights));
  CHECK(weights.objective == -1e6);

  // Every field is addressable by its documented name.
  for (const auto &field : pigpen::agent::reward_weight_fields) {
    RewardWeights target;
    REQUIRE(parse_reward_weight(std::string{field.name} + "=42.5", target));
    CHECK(target.*field.member == 42.5);
  }
}

TEST_CASE("reward weight parsing rejects malformed input unchanged",
          "[reward]") {
  const RewardWeights defaults;
  RewardWeights weights;

  const auto missing_equals = parse_reward_weight("invalid_call", weights);
  REQUIRE_FALSE(missing_equals);
  CHECK(missing_equals.error().contains("NAME=VALUE"));

  const auto unknown = parse_reward_weight("bogus=1", weights);
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().contains("unknown reward weight \"bogus\""));
  CHECK(unknown.error().contains("budget_refused_call"));
  CHECK_FALSE(parse_reward_weight("=1", weights));
  CHECK_FALSE(parse_reward_weight("Score=1", weights));

  for (const auto *const bad :
       {"score=", "score=abc", "score=1.5x", "score= 1", "score=+1",
        "score=inf", "score=nan", "score=1e999", "score=1e308",
        "score=-1000001"}) {
    const auto parsed = parse_reward_weight(bad, weights);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().contains("must be a finite number"));
  }
  CHECK(weights == defaults);
}
