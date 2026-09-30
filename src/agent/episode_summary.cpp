/// @file episode_summary.cpp
/// @brief Episode fact derivation and summaries; the contract is in the
/// header.
#include "agent/episode_summary.hpp"

#include "agent/session.hpp"

#include <array>

namespace pigpen::agent {
namespace {

constexpr std::array item_types{
    world::ItemType::berry,
    world::ItemType::apple,
    world::ItemType::truffle,
    world::ItemType::toadstool,
};

constexpr std::array tool_kinds{
    ToolKind::move,
    ToolKind::look,
    ToolKind::eat,
};

} // namespace

EpisodeFacts episode_facts(const world::World &world,
                           const std::span<const ToolActivity> activities,
                           const std::span<const EpisodeTurn> turns,
                           const EpisodeSnapshot &snapshot) {
  EpisodeFacts facts{
      .finish_reason = snapshot.finish_reason,
      .score = world.score(),
      .observed_cells = static_cast<std::uint32_t>(world.observed_count()),
      .turns_used = snapshot.turns_used,
      .turn_budget = snapshot.turn_budget,
      .executed_actions = static_cast<std::uint32_t>(activities.size()),
  };
  for (const auto &activity : activities) {
    if (!activity.succeeded()) {
      ++facts.failed_actions;
    }
  }
  for (const auto &turn : turns) {
    // Cancelled and errored turns are neither; see EpisodeFacts.
    if (turn.record.status == TurnStatus::completed) {
      if (turn.record.tool_calls > 0U) {
        ++facts.active_turns;
      } else {
        ++facts.zero_tool_turns;
      }
    }
    if (turn.calls) {
      facts.invalid_calls += turn.calls->invalid;
      facts.budget_refused_calls += turn.calls->budget_refused;
      facts.host_refused_calls += turn.calls->host_refused;
    }
  }
  return facts;
}

EpisodeSummary summarize_episode(const world::World &world,
                                 const std::span<const ToolActivity> activities,
                                 const std::span<const EpisodeTurn> turns,
                                 const EpisodeSnapshot &snapshot,
                                 const std::chrono::milliseconds duration,
                                 const RewardWeights &weights) {
  EpisodeSummary summary{
      .finish_reason = snapshot.finish_reason,
      .error = snapshot.error,
      .turns_used = snapshot.turns_used,
      .final_score = world.score(),
      .duration = duration,
      .reward_weights = weights,
      .reward = compute_reward(
          episode_facts(world, activities, turns, snapshot), weights),
  };
  for (const auto item : item_types) {
    summary.items_eaten[std::string{world::item_name(item)}] =
        world.eaten_count(item);
  }
  for (const auto kind : tool_kinds) {
    summary.tool_call_counts[std::string{tool_kind_name(kind)}] = 0U;
  }
  for (const auto &activity : activities) {
    ++summary.tool_call_counts[std::string{tool_kind_name(activity.kind)}];
  }
  for (const auto &turn : turns) {
    if (turn.calls) {
      summary.calls.executed += turn.calls->executed;
      summary.calls.invalid += turn.calls->invalid;
      summary.calls.budget_refused += turn.calls->budget_refused;
      summary.calls.host_refused += turn.calls->host_refused;
    }
  }
  return summary;
}

EpisodeSummary summarize_episode(const Session &session,
                                 const RewardWeights &weights) {
  auto summary = summarize_episode(session.world(), session.tool_activities(),
                                   session.turns(), session.runner().snapshot(),
                                   session.elapsed(), weights);
  summary.rollout_id = session.rollout_id();
  return summary;
}

} // namespace pigpen::agent
