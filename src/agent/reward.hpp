/// @file reward.hpp
/// @brief The shaped, versioned episode reward: plain facts in, a breakdown
/// with raw counts and per-term contributions out.
///
/// The reward is separate from the truthful world score, is computed only
/// from recorded facts, and never feeds back into the world or the prompt.
/// Nothing here depends on scry, JSON, or a Session.
#pragma once

#include "agent/episode_runner.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief Version of compute_reward(); bump whenever a term's definition
/// changes. Recorded as `reward_version` next to every reward.
inline constexpr std::uint32_t reward_version{1};

/// @brief Per-unit weight of each reward term. Every term's contribution is
/// its count times its weight.
struct RewardWeights {
  /// Per point of truthful world score.
  double score{1.0};
  /// Per cell observed beyond the spawn (at most 99).
  double explored_cell{0.05};
  /// Per completed turn with at least one executed world action.
  double active_turn{0.1};
  /// Per completed turn with no executed world action.
  double zero_tool_turn{-1.0};
  /// Per executed world action that did not succeed (wall bump, empty eat).
  double failed_action{-0.1};
  /// Per unknown-tool or undecodable request.
  double invalid_call{-0.5};
  /// Per request past the per-turn call limit.
  double budget_refused_call{-0.25};
  /// Once, when every positive item is eaten.
  double objective{5.0};
  /// Per turn left in the budget, only when the objective is met.
  double unused_turn{0.1};

  friend bool operator==(const RewardWeights &,
                         const RewardWeights &) = default;
};

/// @brief A weight's stable name, shared by `--reward NAME=VALUE`, the
/// `reward_weights` log object, and the key of its term in
/// RewardBreakdown::terms.
struct RewardWeightField {
  std::string_view name;
  double RewardWeights::*member;
};

/// @brief Every weight, in declaration order; reward_term_counts() lists
/// each term's count in this order, and the `--reward` help text
/// (cli::add_reward_option) lists the names from it.
inline constexpr std::array reward_weight_fields{
    RewardWeightField{"score", &RewardWeights::score},
    RewardWeightField{"explored_cell", &RewardWeights::explored_cell},
    RewardWeightField{"active_turn", &RewardWeights::active_turn},
    RewardWeightField{"zero_tool_turn", &RewardWeights::zero_tool_turn},
    RewardWeightField{"failed_action", &RewardWeights::failed_action},
    RewardWeightField{"invalid_call", &RewardWeights::invalid_call},
    RewardWeightField{"budget_refused_call",
                      &RewardWeights::budget_refused_call},
    RewardWeightField{"objective", &RewardWeights::objective},
    RewardWeightField{"unused_turn", &RewardWeights::unused_turn},
};

/// @brief What an episode left behind, as plain counts. Built from a
/// Session's world, activity feed, and retained turns by episode_facts()
/// (episode_summary.hpp); any other source works as long as it keeps these
/// definitions.
struct EpisodeFacts {
  /// Absent while the episode is still running.
  std::optional<FinishReason> finish_reason{};
  /// World::score(), the truthful cumulative score.
  int score{};
  /// World::observed_count(): distinct observed cells, spawn included.
  std::uint32_t observed_cells{1};
  std::uint32_t turns_used{};
  std::uint32_t turn_budget{};
  /// World actions that ran: one per ToolActivity.
  std::uint32_t executed_actions{};
  /// Executed world actions whose outcome is not ToolOutcome::succeeded.
  std::uint32_t failed_actions{};
  /// Completed turns (TurnStatus::completed) with TurnRecord::tool_calls
  /// of at least one. Cancelled and errored turns count as neither active
  /// nor zero-tool, matching the log's `zero_tool_turn`; they only occur in
  /// episodes whose reward is invalid anyway.
  std::uint32_t active_turns{};
  /// Completed turns with TurnRecord::tool_calls of zero.
  std::uint32_t zero_tool_turns{};
  /// TurnCallTally::invalid summed over the turns that have a tally; a turn
  /// without one (no Scry Completion) contributes nothing.
  std::uint32_t invalid_calls{};
  /// TurnCallTally::budget_refused, summed the same way.
  std::uint32_t budget_refused_calls{};
  /// TurnCallTally::host_refused, summed the same way. Recorded, never
  /// penalised: it only happens when the model batched more calls into the
  /// round of the final successful eat.
  std::uint32_t host_refused_calls{};

  friend bool operator==(const EpisodeFacts &, const EpisodeFacts &) = default;
};

/// @brief The reward, its raw counts, and each term's contribution.
struct RewardBreakdown {
  /// True only for FinishReason::turn_budget and objective_complete. An
  /// invalid reward is absent, not zero: trainers drop the episode, and the
  /// log writes `total` as null.
  bool valid{};
  /// Empty when valid; otherwise the finish reason's name (`stopped`,
  /// `cancelled`, `error`) or `unfinished`.
  std::string invalid_reason{};
  /// Sum of terms, always computed so a live view can show the provisional
  /// value; only meaningful when valid.
  double total{};

  // Raw counts, always present so weights can be re-applied offline.
  int score{};
  /// observed_cells - 1: cells observed beyond the spawn.
  std::uint32_t explored_cells{};
  std::uint32_t active_turns{};
  std::uint32_t zero_tool_turns{};
  std::uint32_t failed_actions{};
  std::uint32_t invalid_calls{};
  std::uint32_t budget_refused_calls{};
  /// turn_budget - turns_used when the objective is complete, otherwise 0.
  std::uint32_t unused_turns{};
  /// finish_reason == FinishReason::objective_complete.
  bool objective_complete{};

  /// Count times weight for every term, keyed by weight name (see
  /// reward_weight_fields); `objective` counts once when complete. A zero
  /// contribution is always +0.0, never -0.0.
  std::map<std::string, double> terms{};
};

/// @brief Apply @p weights to @p facts.
[[nodiscard]] RewardBreakdown compute_reward(const EpisodeFacts &facts,
                                             const RewardWeights &weights);

/// @brief The count each term multiplies its weight by, in
/// reward_weight_fields order; `objective` counts 1 when complete, else 0.
[[nodiscard]] std::array<double, reward_weight_fields.size()>
reward_term_counts(const RewardBreakdown &reward) noexcept;

/// @brief Largest accepted weight magnitude. It keeps every term and the
/// total finite: counts are bounded by the world and the turn budget.
inline constexpr double max_reward_weight{1e6};

/// @brief Parse one `NAME=VALUE` override and apply it to @p weights.
/// @param assignment A weight name from reward_weight_fields, `=`, and a
/// finite decimal number with magnitude at most max_reward_weight (negative
/// allowed, no leading `+`; `-0` is stored as `0`).
/// @return A message naming the problem (missing `=`, unknown name, or a
/// value that is not a number in range); @p weights is then unchanged.
[[nodiscard]] std::expected<void, std::string>
parse_reward_weight(std::string_view assignment, RewardWeights &weights);

} // namespace pigpen::agent
