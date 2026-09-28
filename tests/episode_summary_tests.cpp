/// @file episode_summary_tests.cpp
/// @brief Covers episode facts and summaries built from plain values (a
/// World, an activity feed, retained turns, and a runner snapshot) without a
/// Session, and the single JSON serialisation of a summary and of the
/// worker's episode and batch records built on it.

#include "agent/episode_summary.hpp"

#include "agent/summary_json.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using pigpen::agent::EpisodeSnapshot;
using pigpen::agent::EpisodeTurn;
using pigpen::agent::FinishReason;
using pigpen::agent::RunState;
using pigpen::agent::ToolActivity;
using pigpen::agent::ToolKind;
using pigpen::agent::ToolOutcome;
using pigpen::agent::TurnCallTally;
using pigpen::agent::TurnStatus;
using pigpen::world::Direction;
using pigpen::world::ItemType;
using pigpen::world::Position;
using pigpen::world::World;

/// @brief Walk the blob to @p target, east/west first, then north/south.
void walk_to(World &world, const Position target) {
  while (world.position().x != target.x) {
    REQUIRE(world
                .move(world.position().x < target.x ? Direction::east
                                                    : Direction::west)
                .ok);
  }
  while (world.position().y != target.y) {
    REQUIRE(world
                .move(world.position().y < target.y ? Direction::north
                                                    : Direction::south)
                .ok);
  }
}

[[nodiscard]] ToolActivity activity(const ToolKind kind,
                                    const ToolOutcome outcome) {
  return {.kind = kind, .outcome = outcome};
}

[[nodiscard]] EpisodeTurn turn(const TurnStatus status,
                               const std::size_t tool_calls,
                               const std::optional<TurnCallTally> calls) {
  return {.record = {.status = status, .tool_calls = tool_calls},
          .calls = calls};
}

/// @brief A world where the blob walked to the first berry and ate it.
[[nodiscard]] World world_after_eating_a_berry() {
  World world{0};
  const auto items = world.items();
  const auto berry = std::ranges::find(items, ItemType::berry,
                                       &pigpen::world::ItemPlacement::item);
  REQUIRE(berry != items.end());
  walk_to(world, berry->position);
  REQUIRE(world.eat().ok);
  return world;
}

/// @brief Four turns: two active, one zero-tool, one cancelled without a
/// Completion; invalid, over-budget, and host-refused requests throughout.
[[nodiscard]] std::vector<EpisodeTurn> mixed_turns() {
  return {
      turn(TurnStatus::completed, 2,
           TurnCallTally{.executed = 2, .invalid = 1, .budget_refused = 1}),
      turn(TurnStatus::completed, 0, TurnCallTally{.invalid = 1}),
      turn(TurnStatus::completed, 1,
           TurnCallTally{.executed = 1, .host_refused = 2}),
      turn(TurnStatus::cancelled, 0, std::nullopt),
  };
}

[[nodiscard]] std::vector<ToolActivity> mixed_activities() {
  return {
      activity(ToolKind::move, ToolOutcome::succeeded),
      activity(ToolKind::move, ToolOutcome::blocked_by_wall),
      activity(ToolKind::eat, ToolOutcome::nothing_to_eat),
  };
}

} // namespace

TEST_CASE("episode facts derive from world activity and turns", "[summary]") {
  World world{0};
  REQUIRE(world.move(Direction::east).ok);
  const auto activities = mixed_activities();
  const auto turns = mixed_turns();
  const EpisodeSnapshot snapshot{
      .state = RunState::finished,
      .turns_used = 4,
      .turn_budget = 6,
      .finish_reason = FinishReason::turn_budget,
  };

  const auto facts =
      pigpen::agent::episode_facts(world, activities, turns, snapshot);
  CHECK(facts == pigpen::agent::EpisodeFacts{
                     .finish_reason = FinishReason::turn_budget,
                     .score = 0,
                     .observed_cells = 2,
                     .turns_used = 4,
                     .turn_budget = 6,
                     .executed_actions = 3,
                     .failed_actions = 2,
                     .active_turns = 2,
                     .zero_tool_turns = 1,
                     .invalid_calls = 2,
                     .budget_refused_calls = 1,
                     .host_refused_calls = 2,
                 });
}

TEST_CASE("a cancelled or errored turn is neither active nor zero-tool",
          "[summary]") {
  const World world{0};
  const std::vector<EpisodeTurn> turns{
      turn(TurnStatus::cancelled, 1, std::nullopt),
      turn(TurnStatus::error, 0, std::nullopt),
  };
  const auto facts =
      pigpen::agent::episode_facts(world, {}, turns, EpisodeSnapshot{});
  CHECK(facts.active_turns == 0);
  CHECK(facts.zero_tool_turns == 0);
  CHECK_FALSE(facts.finish_reason);
}

TEST_CASE("a summary reports counts tallies and the reward", "[summary]") {
  const auto world = world_after_eating_a_berry();
  auto activities = mixed_activities();
  activities.push_back(activity(ToolKind::eat, ToolOutcome::succeeded));
  activities.push_back(activity(ToolKind::look, ToolOutcome::succeeded));
  const auto turns = mixed_turns();
  const EpisodeSnapshot snapshot{
      .state = RunState::finished,
      .turns_used = 4,
      .turn_budget = 4,
      .finish_reason = FinishReason::turn_budget,
  };
  pigpen::agent::RewardWeights weights;
  weights.invalid_call = -2.0;

  const auto summary = pigpen::agent::summarize_episode(
      world, activities, turns, snapshot, std::chrono::milliseconds{1234},
      weights);
  CHECK(summary.complete());
  CHECK(summary.finish_reason == FinishReason::turn_budget);
  CHECK(summary.error.empty());
  CHECK(summary.turns_used == 4);
  CHECK(summary.final_score == 1);
  CHECK(summary.items_eaten ==
        std::map<std::string, std::size_t>{
            {"apple", 0}, {"berry", 1}, {"toadstool", 0}, {"truffle", 0}});
  CHECK(summary.tool_call_counts == std::map<std::string, std::size_t>{
                                        {"eat", 2}, {"look", 1}, {"move", 2}});
  CHECK(summary.calls == TurnCallTally{.executed = 3,
                                       .invalid = 2,
                                       .budget_refused = 1,
                                       .host_refused = 2});
  CHECK(summary.duration == std::chrono::milliseconds{1234});
  CHECK(summary.reward_weights == weights);
  // The reward is compute_reward() over the same facts and weights.
  const auto expected = pigpen::agent::compute_reward(
      pigpen::agent::episode_facts(world, activities, turns, snapshot),
      weights);
  CHECK(summary.reward.valid);
  CHECK(summary.reward.terms == expected.terms);
  CHECK(summary.reward.total == expected.total);
  CHECK(summary.reward.invalid_calls == 2);
  CHECK(summary.reward.terms.at("invalid_call") == -4.0);
}

TEST_CASE("an unfinished summary is incomplete with an invalid reward",
          "[summary]") {
  const World world{0};
  const EpisodeSnapshot snapshot{
      .state = RunState::playing, .turns_used = 1, .turn_budget = 3};
  const auto summary = pigpen::agent::summarize_episode(
      world, {}, {}, snapshot, std::chrono::milliseconds{5}, {});
  CHECK_FALSE(summary.complete());
  CHECK_FALSE(summary.reward.valid);
  CHECK(summary.reward.invalid_reason == "unfinished");
  // Every tool and item is listed even when nothing happened.
  CHECK(summary.items_eaten.size() == 4);
  CHECK(summary.tool_call_counts == std::map<std::string, std::size_t>{
                                        {"eat", 0}, {"look", 0}, {"move", 0}});
  CHECK(summary.calls == TurnCallTally{});
}

TEST_CASE("a summary serialises to one JSON line", "[summary][json]") {
  const World world{0};
  const auto activities = mixed_activities();
  const auto turns = mixed_turns();
  EpisodeSnapshot snapshot{
      .state = RunState::finished,
      .turns_used = 4,
      .turn_budget = 4,
      .finish_reason = FinishReason::turn_budget,
  };
  const auto summary = pigpen::agent::summarize_episode(
      world, activities, turns, snapshot, std::chrono::milliseconds{77}, {});

  const auto line = pigpen::agent::to_json_line(summary, "footer");
  CHECK_FALSE(line.contains('\n'));
  const auto record = nlohmann::json::parse(line);
  std::vector<std::string> keys;
  for (const auto &[key, value] : record.items()) {
    keys.push_back(key);
  }
  CHECK(keys == std::vector<std::string>{
                    "calls", "complete", "duration_ms", "error", "final_score",
                    "finish_reason", "items_eaten", "reward", "reward_version",
                    "reward_weights", "rollout_id", "tool_call_counts",
                    "turns_used", "type"});
  CHECK(record.at("type") == "footer");
  // Built without a Session, the summary has no rollout id.
  CHECK(record.at("rollout_id").is_null());
  CHECK(record.at("complete") == true);
  CHECK(record.at("finish_reason") == "turn_budget");
  CHECK(record.at("error") == "");
  CHECK(record.at("final_score") == 0);
  CHECK(record.at("turns_used") == 4);
  CHECK(record.at("duration_ms") == 77);
  CHECK(record.at("tool_call_counts") ==
        nlohmann::json{{"eat", 1}, {"look", 0}, {"move", 2}});
  CHECK(record.at("calls") == nlohmann::json{{"executed", 3},
                                             {"invalid", 2},
                                             {"budget_refused", 1},
                                             {"host_refused", 2}});
  CHECK(record.at("reward_version") == 1);
  CHECK(record.at("reward_weights") ==
        nlohmann::json{{"score", 1.0},
                       {"explored_cell", 0.05},
                       {"active_turn", 0.1},
                       {"zero_tool_turn", -1.0},
                       {"failed_action", -0.1},
                       {"invalid_call", -0.5},
                       {"budget_refused_call", -0.25},
                       {"objective", 5.0},
                       {"unused_turn", 0.1}});

  const auto &reward = record.at("reward");
  CHECK(reward.at("valid") == true);
  CHECK(reward.at("invalid_reason").is_null());
  // 2 active turns, 1 zero-tool turn, 2 failed, 2 invalid, 1 over budget.
  CHECK(reward.at("total").get<double>() ==
        Catch::Approx(0.2 - 1.0 - 0.2 - 1.0 - 0.25));
  CHECK(reward.at("score") == 0);
  CHECK(reward.at("explored_cells") == 0);
  CHECK(reward.at("active_turns") == 2);
  CHECK(reward.at("zero_tool_turns") == 1);
  CHECK(reward.at("failed_actions") == 2);
  CHECK(reward.at("invalid_calls") == 2);
  CHECK(reward.at("budget_refused_calls") == 1);
  CHECK(reward.at("unused_turns") == 0);
  CHECK(reward.at("objective_complete") == false);
  CHECK(reward.at("terms").size() == 9);
  CHECK(reward.at("terms").at("zero_tool_turn") == -1.0);
  // Zero contributions are written as 0.0, never -0.0.
  CHECK(record.dump().find("-0.0") == std::string::npos);

  // An invalid reward has no total but keeps its counts and terms.
  snapshot.finish_reason = FinishReason::error;
  snapshot.error = "transport failed";
  const auto failed = nlohmann::json::parse(pigpen::agent::to_json_line(
      pigpen::agent::summarize_episode(world, activities, turns, snapshot,
                                       std::chrono::milliseconds{1}, {})));
  CHECK(failed.at("type") == "summary");
  CHECK(failed.at("finish_reason") == "error");
  CHECK(failed.at("error") == "transport failed");
  CHECK(failed.at("reward").at("valid") == false);
  CHECK(failed.at("reward").at("invalid_reason") == "error");
  CHECK(failed.at("reward").at("total").is_null());
  CHECK(failed.at("reward").at("invalid_calls") == 2);
  CHECK(failed.at("reward").at("terms").at("invalid_call") == -1.0);

  // A summary taken mid-episode records no finish reason.
  snapshot.finish_reason.reset();
  const auto running = nlohmann::json::parse(pigpen::agent::to_json_line(
      pigpen::agent::summarize_episode(world, activities, turns, snapshot,
                                       std::chrono::milliseconds{1}, {})));
  CHECK(running.at("complete") == false);
  CHECK(running.at("finish_reason").is_null());
  CHECK(running.at("reward").at("invalid_reason") == "unfinished");
}

TEST_CASE("a worker episode record is the summary plus its identity",
          "[summary][json]") {
  const World world{0};
  const auto turns = mixed_turns();
  const EpisodeSnapshot snapshot{
      .state = RunState::finished,
      .turns_used = 4,
      .turn_budget = 4,
      .finish_reason = FinishReason::turn_budget,
  };
  pigpen::agent::EpisodeRecord record{
      .summary = pigpen::agent::summarize_episode(
          world, mixed_activities(), turns, snapshot,
          std::chrono::milliseconds{41}, {}),
      .sample = 2,
  };
  record.summary.rollout_id = "run42/1003/2";
  record.config.model = "m";
  record.config.seed = 1003;

  const auto line = pigpen::agent::to_json_line(record);
  CHECK_FALSE(line.contains('\n'));
  auto episode = nlohmann::json::parse(line);
  auto footer =
      nlohmann::json::parse(pigpen::agent::to_json_line(record.summary));
  CHECK(episode.at("type") == "episode");
  CHECK(episode.at("rollout_id") == "run42/1003/2");
  CHECK(episode.at("seed") == 1003);
  CHECK(episode.at("sample") == 2);
  CHECK(episode.at("sampling_seed").is_null());
  CHECK(episode.at("config").at("seed") == 1003);
  CHECK(episode.at("config").at("model") == "m");
  CHECK(episode.at("config").at("sampling_seed").is_null());
  CHECK(episode.at("config").at("scenario").at("turn_budget") == 20);
  // Apart from its type and the four identity keys, the record is exactly
  // the summary's line.
  for (const auto *const key : {"seed", "sample", "sampling_seed", "config"}) {
    CHECK(episode.erase(key) == 1);
  }
  episode.erase("type");
  footer.erase("type");
  CHECK(episode == footer);

  record.config.sampling_seed = 9;
  const auto seeded =
      nlohmann::json::parse(pigpen::agent::to_json_line(record));
  CHECK(seeded.at("sampling_seed") == 9);
  CHECK(seeded.at("config").at("sampling_seed") == 9);
}

TEST_CASE("a batch record serialises every count", "[summary][json]") {
  const auto completed = nlohmann::json::parse(
      pigpen::agent::to_json_line(pigpen::agent::BatchRecord{
          .status = "completed",
          .jobs = 6,
          .episodes = 6,
          .valid = 5,
          .invalid = 1,
          .duration = std::chrono::milliseconds{1234},
          .exit_code = 6,
      }));
  CHECK(completed == nlohmann::json{{"type", "batch"},
                                    {"status", "completed"},
                                    {"jobs", 6},
                                    {"episodes", 6},
                                    {"valid", 5},
                                    {"invalid", 1},
                                    {"not_started", 0},
                                    {"duration_ms", 1234},
                                    {"error", nullptr},
                                    {"exit_code", 6}});
  const auto aborted = nlohmann::json::parse(
      pigpen::agent::to_json_line(pigpen::agent::BatchRecord{
          .status = "aborted",
          .jobs = 3,
          .not_started = 3,
          .error = "rollout/1/0: bad header",
          .exit_code = 1,
      }));
  CHECK(aborted.at("error") == "rollout/1/0: bad header");
  CHECK(aborted.at("not_started") == 3);
}

TEST_CASE("records never throw on text that is not UTF-8", "[summary][json]") {
  pigpen::agent::EpisodeRecord record{
      .summary = {.error = "bad \xff byte", .rollout_id = "r/1/0"},
  };
  record.config.model = "pig\xc3";
  std::string line;
  REQUIRE_NOTHROW(line = pigpen::agent::to_json_line(record));
  const auto parsed = nlohmann::json::parse(line);
  // Each invalid byte becomes U+FFFD; the rest of the text survives.
  CHECK(parsed.at("error") == "bad \xef\xbf\xbd byte");
  CHECK(parsed.at("config").at("model") == "pig\xef\xbf\xbd");
  REQUIRE_NOTHROW(line = pigpen::agent::to_json_line(record.summary));
  CHECK(nlohmann::json::parse(line).at("error") == "bad \xef\xbf\xbd byte");
  REQUIRE_NOTHROW(line = pigpen::agent::to_json_line(
                      pigpen::agent::BatchRecord{.error = "x\xff"}));
  CHECK(nlohmann::json::parse(line).at("error") == "x\xef\xbf\xbd");
}
