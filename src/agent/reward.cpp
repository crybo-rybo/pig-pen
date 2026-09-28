/// @file reward.cpp
/// @brief Reward computation and weight parsing; the contract is in the
/// header.
#include "agent/reward.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>

namespace pigpen::agent {
namespace {

/// @brief Every valid weight name, for error messages.
[[nodiscard]] std::string weight_names() {
  std::string names;
  for (const auto &field : reward_weight_fields) {
    if (!names.empty()) {
      names += ", ";
    }
    names += field.name;
  }
  return names;
}

} // namespace

RewardBreakdown compute_reward(const EpisodeFacts &facts,
                               const RewardWeights &weights) {
  RewardBreakdown reward{
      .score = facts.score,
      .explored_cells =
          facts.observed_cells > 0U ? facts.observed_cells - 1U : 0U,
      .active_turns = facts.active_turns,
      .zero_tool_turns = facts.zero_tool_turns,
      .failed_actions = facts.failed_actions,
      .invalid_calls = facts.invalid_calls,
      .budget_refused_calls = facts.budget_refused_calls,
      .objective_complete =
          facts.finish_reason == FinishReason::objective_complete,
  };
  if (reward.objective_complete && facts.turn_budget > facts.turns_used) {
    reward.unused_turns = facts.turn_budget - facts.turns_used;
  }

  if (!facts.finish_reason) {
    reward.invalid_reason = "unfinished";
  } else if (const auto reason = *facts.finish_reason;
             reason == FinishReason::turn_budget ||
             reason == FinishReason::objective_complete) {
    reward.valid = true;
  } else {
    reward.invalid_reason = finish_reason_name(reason);
  }

  const auto counts = reward_term_counts(reward);
  for (std::size_t index = 0; index < counts.size(); ++index) {
    const auto &field = reward_weight_fields[index];
    // Adding +0.0 turns a -0.0 product (zero count, negative weight) into
    // +0.0, so logs never show "-0.0".
    const auto contribution = counts[index] * weights.*field.member + 0.0;
    reward.terms.emplace(field.name, contribution);
    reward.total += contribution;
  }
  return reward;
}

std::array<double, reward_weight_fields.size()>
reward_term_counts(const RewardBreakdown &reward) noexcept {
  return {
      static_cast<double>(reward.score),
      static_cast<double>(reward.explored_cells),
      static_cast<double>(reward.active_turns),
      static_cast<double>(reward.zero_tool_turns),
      static_cast<double>(reward.failed_actions),
      static_cast<double>(reward.invalid_calls),
      static_cast<double>(reward.budget_refused_calls),
      reward.objective_complete ? 1.0 : 0.0,
      static_cast<double>(reward.unused_turns),
  };
}

std::expected<void, std::string>
parse_reward_weight(const std::string_view assignment, RewardWeights &weights) {
  const auto equals = assignment.find('=');
  if (equals == std::string_view::npos) {
    return std::unexpected("reward weight \"" + std::string{assignment} +
                           "\" must have the form NAME=VALUE");
  }
  const auto name = assignment.substr(0, equals);
  const auto value = assignment.substr(equals + 1);
  const auto field =
      std::ranges::find(reward_weight_fields, name, &RewardWeightField::name);
  if (field == reward_weight_fields.end()) {
    return std::unexpected("unknown reward weight \"" + std::string{name} +
                           "\"; expected one of: " + weight_names());
  }
  double number{};
  const auto *const end = value.data() + value.size();
  if (const auto [last, error] = std::from_chars(value.data(), end, number);
      value.empty() || error != std::errc{} || last != end ||
      !std::isfinite(number)) {
    return std::unexpected("reward weight " + std::string{name} +
                           " must be a finite number, got \"" +
                           std::string{value} + '"');
  }
  weights.*field->member = number;
  return {};
}

} // namespace pigpen::agent
