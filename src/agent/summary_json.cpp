/// @file summary_json.cpp
/// @brief Summary and record serialisation; the contracts are in
/// summary_json.hpp and record_json.hpp.
#include "agent/summary_json.hpp"

#include "agent/record_json.hpp"
#include "world/world.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

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

/// @brief @p value, or null when it is empty.
[[nodiscard]] nlohmann::json nullable(const std::string &value) {
  return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

[[nodiscard]] nlohmann::json
sampling_seed_json(const std::optional<std::uint32_t> &seed) {
  return seed ? nlohmann::json(*seed) : nlohmann::json(nullptr);
}

/// @brief @p summary as the object to_json_line() writes, so a record can
/// extend it with keys of its own.
[[nodiscard]] nlohmann::json summary_json(const EpisodeSummary &summary,
                                          const std::string_view record_type) {
  return {
      {"type", record_type},
      {"rollout_id", nullable(summary.rollout_id)},
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
}

} // namespace

nlohmann::json position_json(const world::Position position) {
  return {{"x", position.x}, {"y", position.y}};
}

nlohmann::json config_json(const Config &config) {
  return {
      {"model", config.model},
      {"base_url", config.base_url},
      {"temperature", config.temperature},
      {"max_output_tokens", config.max_output_tokens},
      {"seed", config.seed},
      {"sampling_seed", sampling_seed_json(config.sampling_seed)},
      {"scenario",
       {
           {"grid",
            {{"width", world::World::width}, {"height", world::World::height}}},
           {"spawn", position_json(world::World::spawn)},
           {"items",
            {{"berry", world::World::default_berry_count},
             {"apple", world::World::default_apple_count},
             {"truffle", world::World::default_truffle_count},
             {"toadstool", world::World::default_toadstool_count}}},
           {"turn_budget", config.turn_budget},
           {"max_tool_rounds", config.max_tool_rounds},
           {"max_world_tool_calls_per_turn", max_world_tool_calls_per_turn},
           {"known_item_values", config.known_item_values},
           {"reward_feedback", config.reward_feedback},
           {"opaque_look", config.opaque_look},
       }},
  };
}

std::string to_json_line(const EpisodeSummary &summary,
                         const std::string_view record_type) {
  return summary_json(summary, record_type).dump();
}

std::string to_json_line(const EpisodeRecord &record) {
  auto line = summary_json(record.summary, "episode");
  line["seed"] = record.config.seed;
  line["sample"] = record.sample;
  line["sampling_seed"] = sampling_seed_json(record.config.sampling_seed);
  line["config"] = config_json(record.config);
  return line.dump();
}

std::string to_json_line(const BatchRecord &record) {
  const nlohmann::json line = {
      {"type", "batch"},
      {"status", record.status},
      {"jobs", record.jobs},
      {"episodes", record.episodes},
      {"valid", record.valid},
      {"invalid", record.invalid},
      {"not_started", record.not_started},
      {"duration_ms", record.duration.count()},
      {"error", nullable(record.error)},
      {"exit_code", record.exit_code},
  };
  return line.dump();
}

} // namespace pigpen::agent
