/// @file episode_summary.hpp
/// @brief One episode's outcome as plain data: what the log footer records
/// and what front ends report, built from a Session's facts on demand.
#pragma once

#include "agent/episode_runner.hpp"
#include "agent/episode_turn.hpp"
#include "agent/events.hpp"
#include "agent/reward.hpp"
#include "world/world.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace pigpen::agent {

class Session;

/// @brief Everything known about an episode's outcome. to_json_line()
/// (summary_json.hpp) is its only serialisation.
struct EpisodeSummary {
  /// Absent while the episode is still running.
  std::optional<FinishReason> finish_reason{};
  /// The runner's terminal error text, or empty.
  std::string error{};
  /// SessionOptions::rollout_id of the session summarised, or empty; the
  /// plain-facts summarize_episode() leaves it empty.
  std::string rollout_id{};
  std::uint32_t turns_used{};
  /// Truthful world score.
  int final_score{};
  /// Eaten count per item name; every item kind is listed, even at zero.
  std::map<std::string, std::size_t> items_eaten{};
  /// Executed world actions per tool name; every tool is listed, even at 0.
  std::map<std::string, std::size_t> tool_call_counts{};
  /// Call tallies summed over the turns that have one.
  TurnCallTally calls{};
  /// Wall time from session creation to the finish (or to now, unfinished).
  std::chrono::milliseconds duration{};
  /// The weights @ref reward was computed with.
  RewardWeights reward_weights{};
  RewardBreakdown reward{};

  /// @brief Whether the episode reached a terminal state on its own.
  [[nodiscard]] bool complete() const noexcept {
    return finish_reason.has_value();
  }
};

/// @brief Derive reward facts from an episode's recorded state. See
/// EpisodeFacts for each count's definition.
/// @param snapshot Supplies the finish reason, turns used, and turn budget.
[[nodiscard]] EpisodeFacts episode_facts(
    const world::World &world, std::span<const ToolActivity> activities,
    std::span<const EpisodeTurn> turns, const EpisodeSnapshot &snapshot);

/// @brief Summarise an episode from plain facts, with no Session involved.
[[nodiscard]] EpisodeSummary summarize_episode(
    const world::World &world, std::span<const ToolActivity> activities,
    std::span<const EpisodeTurn> turns, const EpisodeSnapshot &snapshot,
    std::chrono::milliseconds duration, const RewardWeights &weights);

/// @brief Summarise @p session as it stands, including its rollout id;
/// callable at any time, including mid-episode (the reward is then invalid
/// with reason `unfinished`).
[[nodiscard]] EpisodeSummary summarize_episode(const Session &session,
                                               const RewardWeights &weights);

} // namespace pigpen::agent
