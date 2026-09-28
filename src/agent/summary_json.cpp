/// @file summary_json.cpp
/// @brief EpisodeSummary serialisation; the contract is in the header.
#include "agent/summary_json.hpp"

#include <nlohmann/json.hpp>

namespace pigpen::agent {
namespace {

[[nodiscard]] nlohmann::json tally_json(const TurnCallTally &tally) {
  return {{"executed", tally.executed},
          {"invalid", tally.invalid},
          {"budget_refused", tally.budget_refused},
          {"host_refused", tally.host_refused}};
}

[[nodiscard]] nlohmann::json weights_json(const RewardWeights &weights) {
  auto result = nlohmann::json::object();
  for (const auto &field : reward_weight_fields) {
    result[std::string{field.name}] = weights.*field.member;
  }
  return result;
}

[[nodiscard]] nlohmann::json reward_json(const RewardBreakdown &reward) {
  return {
      {"valid", reward.valid},
      {"invalid_reason", reward.valid ? nlohmann::json(nullptr)
                                      : nlohmann::json(reward.invalid_reason)},
      {"total",
       reward.valid ? nlohmann::json(reward.total) : nlohmann::json(nullptr)},
      {"score", reward.score},
      {"explored_cells", reward.explored_cells},
      {"active_turns", reward.active_turns},
      {"zero_tool_turns", reward.zero_tool_turns},
      {"failed_actions", reward.failed_actions},
      {"invalid_calls", reward.invalid_calls},
      {"budget_refused_calls", reward.budget_refused_calls},
      {"unused_turns", reward.unused_turns},
      {"objective_complete", reward.objective_complete},
      {"terms", reward.terms},
  };
}

} // namespace

std::string to_json_line(const EpisodeSummary &summary,
                         const std::string_view record_type) {
  const nlohmann::json record = {
      {"type", record_type},
      {"complete", summary.complete()},
      {"finish_reason",
       summary.finish_reason
           ? nlohmann::json(finish_reason_name(*summary.finish_reason))
           : nlohmann::json(nullptr)},
      {"error", summary.error},
      {"final_score", summary.final_score},
      {"items_eaten", summary.items_eaten},
      {"tool_call_counts", summary.tool_call_counts},
      {"turns_used", summary.turns_used},
      {"duration_ms", summary.duration.count()},
      {"calls", tally_json(summary.calls)},
      {"reward", reward_json(summary.reward)},
      {"reward_version", reward_version},
      {"reward_weights", weights_json(summary.reward_weights)},
  };
  return record.dump();
}

} // namespace pigpen::agent
