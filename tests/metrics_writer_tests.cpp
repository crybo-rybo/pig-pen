/// @file metrics_writer_tests.cpp
/// @brief Covers JSONL header/tool/turn/footer reconciliation, the footer's
/// reward fields, the incomplete "abandoned" footer emitted on destruction,
/// footer finality, and the Config serialisation the header shares with the
/// worker's episode record.

#include "agent/metrics_writer.hpp"

#include "agent/summary_json.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

/// @brief Parses every line of a JSONL log so assertions can address records
/// by position and field rather than by raw text.
[[nodiscard]] std::vector<nlohmann::json>
read_records(const std::filesystem::path &path) {
  std::ifstream stream{path};
  std::vector<nlohmann::json> records;
  for (std::string line; std::getline(stream, line);) {
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

/// @brief Yields a unique temp-directory path per invocation so concurrent
/// test runs never share a log directory.
[[nodiscard]] std::filesystem::path test_directory() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("pigpen-metrics-tests-" + std::to_string(stamp));
}

/// @brief A finished episode summary with nothing recorded.
[[nodiscard]] pigpen::agent::EpisodeSummary
finished_summary(const pigpen::agent::FinishReason reason) {
  pigpen::agent::EpisodeFacts facts{.finish_reason = reason};
  return {
      .finish_reason = reason,
      .items_eaten = {{"apple", 0},
                      {"berry", 0},
                      {"toadstool", 0},
                      {"truffle", 0}},
      .tool_call_counts = {{"eat", 0}, {"look", 0}, {"move", 0}},
      .reward = pigpen::agent::compute_reward(facts, {}),
  };
}

} // namespace

TEST_CASE("metrics log contains a reconcilable header tool turn and footer") {
  const auto directory = test_directory();
  pigpen::agent::Config config;
  config.seed = 42;
  config.turn_budget = 2;
  config.temperature = 0.5;
  config.sampling_seed = 7;
  auto created =
      pigpen::agent::MetricsWriter::create(directory, config, "default");
  REQUIRE(created.has_value());
  auto writer = std::move(*created);
  const auto path = writer->path();

  const pigpen::agent::ToolActivity activity{
      .tick = 1,
      .turn = 1,
      .kind = pigpen::agent::ToolKind::eat,
      .outcome = pigpen::agent::ToolOutcome::succeeded,
      .arguments_json = "{}",
      .result_json =
          R"({"ate":"berry","ok":true,"reason":null,"reward":1,"score":1})",
      .before = {.x = 4, .y = 4},
      .after = {.x = 4, .y = 4},
      .eaten = pigpen::world::ItemType::berry,
      .score_after = 1,
      .scry_turn_id = 7,
      .call_id = "eat-1",
      .round = 2,
      .index = 1,
  };
  REQUIRE(writer->record_tool(activity).has_value());
  REQUIRE(writer
              ->record_turn({
                  .record =
                      {
                          .turn = 1,
                          .status = pigpen::agent::TurnStatus::completed,
                          .assistant_text = "ate a berry",
                          .tool_calls = 1,
                          .latency = std::chrono::milliseconds{12},
                          .tool_stats =
                              pigpen::agent::TurnToolStats{
                                  .rounds = 2,
                                  .calls = 4,
                                  .rejected_calls = 1,
                                  .round_limit_reached = true,
                                  .unexecuted_calls = 2},
                      },
                  .calls = pigpen::agent::TurnCallTally{.executed = 1,
                                                        .invalid = 2,
                                                        .budget_refused = 1,
                                                        .host_refused = 0},
              })
              .has_value());
  pigpen::agent::RewardWeights weights;
  weights.invalid_call = -1.0;
  const pigpen::agent::EpisodeFacts facts{
      .finish_reason = pigpen::agent::FinishReason::turn_budget,
      .score = 1,
      .observed_cells = 3,
      .turns_used = 1,
      .turn_budget = 2,
      .executed_actions = 1,
      .active_turns = 1,
      .invalid_calls = 2,
      .budget_refused_calls = 1,
  };
  REQUIRE(writer
              ->finish({
                  .finish_reason = pigpen::agent::FinishReason::turn_budget,
                  .turns_used = 1,
                  .final_score = 1,
                  .items_eaten = {{"apple", 0},
                                  {"berry", 1},
                                  {"toadstool", 0},
                                  {"truffle", 0}},
                  .tool_call_counts = {{"eat", 1}, {"look", 0}, {"move", 0}},
                  .calls = {.executed = 1, .invalid = 2, .budget_refused = 1},
                  .duration = std::chrono::milliseconds{250},
                  .reward_weights = weights,
                  .reward = pigpen::agent::compute_reward(facts, weights),
              })
              .has_value());

  const auto records = read_records(path);
  REQUIRE(records.size() == 4);
  REQUIRE(records.front().at("type") == "header");
  REQUIRE(records.front().at("seed") == 42);
  REQUIRE(records.front().at("temperature") == 0.5);
  REQUIRE(records.front().at("sampling_seed") == 7);
  REQUIRE(records.front().at("max_output_tokens") == config.max_output_tokens);
  REQUIRE(records.front().at("scenario").at("max_world_tool_calls_per_turn") ==
          4);
  REQUIRE(records[1].at("type") == "tool");
  REQUIRE(records[1].at("args") == nlohmann::json::object());
  REQUIRE(records[1].at("action_executed") == true);
  CHECK(records[1].at("scry_turn_id") == 7);
  CHECK(records[1].at("call_id") == "eat-1");
  CHECK(records[1].at("round") == 2);
  CHECK(records[1].at("index") == 1);
  REQUIRE(records[1].at("result") ==
          nlohmann::json::parse(activity.result_json));
  REQUIRE(records[2].at("type") == "turn");
  REQUIRE(records[2].at("tool_calls") == 1);
  REQUIRE(records[2].at("zero_tool_turn") == false);
  CHECK(records[2].at("scry_tools") ==
        nlohmann::json{{"rounds", 2},
                       {"calls", 4},
                       {"rejected_calls", 1},
                       {"round_limit_reached", true},
                       {"unexecuted_calls", 2}});
  CHECK(records[2].at("calls") == nlohmann::json{{"executed", 1},
                                                 {"invalid", 2},
                                                 {"budget_refused", 1},
                                                 {"host_refused", 0}});
  // The tally is additive: every earlier turn field is still written.
  std::vector<std::string> turn_keys;
  for (const auto &[key, value] : records[2].items()) {
    turn_keys.push_back(key);
  }
  CHECK(turn_keys ==
        std::vector<std::string>{"assistant_text", "calls", "error",
                                 "input_tokens", "latency_ms", "output_tokens",
                                 "scry_tools", "status", "tool_calls", "turn",
                                 "type", "user_message", "zero_tool_turn"});
  const auto &footer = records.back();
  REQUIRE(footer.at("type") == "footer");
  REQUIRE(footer.at("complete") == true);
  CHECK(footer.at("finish_reason") == "turn_budget");
  CHECK(footer.at("error") == "");
  REQUIRE(footer.at("final_score") == 1);
  REQUIRE(footer.at("items_eaten").at("berry") == 1);
  REQUIRE(footer.at("tool_call_counts").at("eat") == 1);
  CHECK(footer.at("turns_used") == 1);
  CHECK(footer.at("duration_ms") == 250);
  // The reward fields are additive: every earlier footer field is still
  // written under the same name.
  std::vector<std::string> footer_keys;
  for (const auto &[key, value] : footer.items()) {
    footer_keys.push_back(key);
  }
  CHECK(footer_keys ==
        std::vector<std::string>{"calls", "complete", "duration_ms", "error",
                                 "final_score", "finish_reason", "items_eaten",
                                 "reward", "reward_version", "reward_weights",
                                 "rollout_id", "tool_call_counts", "turns_used",
                                 "type"});
  CHECK(footer.at("calls") == nlohmann::json{{"executed", 1},
                                             {"invalid", 2},
                                             {"budget_refused", 1},
                                             {"host_refused", 0}});
  CHECK(footer.at("reward_version") == 1);
  CHECK(footer.at("reward_weights").at("invalid_call") == -1.0);
  CHECK(footer.at("reward_weights").at("score") == 1.0);
  CHECK(footer.at("reward_weights").size() == 9);
  const auto &reward = footer.at("reward");
  CHECK(reward.at("valid") == true);
  CHECK(reward.at("invalid_reason").is_null());
  // 1 score + 2 explored cells + 1 active turn - 2 invalid - 1 over budget.
  CHECK(reward.at("total").get<double>() ==
        Catch::Approx(1.0 + 0.1 + 0.1 - 2.0 - 0.25));
  CHECK(reward.at("explored_cells") == 2);
  CHECK(reward.at("invalid_calls") == 2);
  CHECK(reward.at("terms").at("invalid_call") == -2.0);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("destroying an unfinished writer still emits an incomplete footer") {
  const auto directory = test_directory();
  auto created = pigpen::agent::MetricsWriter::create(
      directory, pigpen::agent::Config{}, "default");
  REQUIRE(created.has_value());
  const auto path = (*created)->path();
  REQUIRE((*created)
              ->record_turn({
                  .record =
                      {
                          .turn = 3,
                          .status = pigpen::agent::TurnStatus::completed,
                          .assistant_text = "partial run",
                      },
              })
              .has_value());
  created->reset();

  const auto records = read_records(path);
  REQUIRE(records.size() == 3);
  // No Completion statistics means no tally, written as an explicit null.
  CHECK(records[1].at("scry_tools").is_null());
  REQUIRE(records[1].contains("calls"));
  CHECK(records[1].at("calls").is_null());
  REQUIRE(records.front().at("sampling_seed").is_null());
  REQUIRE(records.back().at("type") == "footer");
  REQUIRE(records.back().at("complete") == false);
  REQUIRE(records.back().at("finish_reason") == "abandoned");
  REQUIRE(records.back().at("turns_used") == 3);
  CHECK(records.back().at("rollout_id").is_null());
  // An abandoned episode has no summary, so no reward fields.
  CHECK_FALSE(records.back().contains("reward"));
  CHECK_FALSE(records.back().contains("calls"));

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("metrics footer is final and cannot be duplicated") {
  const auto directory = test_directory();
  auto created = pigpen::agent::MetricsWriter::create(
      directory, pigpen::agent::Config{}, "finalization-test");
  REQUIRE(created.has_value());
  auto writer = std::move(*created);
  const auto path = writer->path();

  // A summary of an unfinished episode is refused without writing.
  auto unfinished = finished_summary(pigpen::agent::FinishReason::stopped);
  unfinished.finish_reason.reset();
  CHECK_FALSE(writer->finish(unfinished));

  REQUIRE(writer->finish(finished_summary(pigpen::agent::FinishReason::stopped))
              .has_value());
  CHECK_FALSE(
      writer->finish(finished_summary(pigpen::agent::FinishReason::stopped)));
  CHECK_FALSE(writer->record_turn({.record = {.turn = 1}}));

  const auto records = read_records(path);
  REQUIRE(records.size() == 2);
  CHECK(records.back().at("type") == "footer");
  CHECK(records.back().at("finish_reason") == "stopped");
  CHECK(records.back().at("complete") == true);
  // A stopped episode's reward is absent, not zero.
  CHECK(records.back().at("reward").at("valid") == false);
  CHECK(records.back().at("reward").at("invalid_reason") == "stopped");
  CHECK(records.back().at("reward").at("total").is_null());

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("the header and the worker's episode record share one Config "
          "serialisation") {
  const auto directory = test_directory();
  pigpen::agent::Config config;
  config.model = "registry.example/pig-model:Q4_K_M";
  config.seed = 1003;
  config.turn_budget = 9;
  config.sampling_seed = 12;
  config.opaque_look = true;
  auto created = pigpen::agent::MetricsWriter::create(
      directory, config, "default", "run42/1003/2");
  REQUIRE(created.has_value());
  const auto path = (*created)->path();
  auto summary = finished_summary(pigpen::agent::FinishReason::turn_budget);
  summary.rollout_id = "run42/1003/2";
  REQUIRE((*created)->finish(summary));
  created->reset();

  const auto records = read_records(path);
  REQUIRE(records.size() == 2);
  auto header = records.front();
  CHECK(header.at("rollout_id") == "run42/1003/2");
  CHECK(records.back().at("rollout_id") == "run42/1003/2");
  // Everything in the header but its own four keys is the Config.
  for (const auto *const key :
       {"type", "started_at", "prompt_variant", "rollout_id"}) {
    CHECK(header.erase(key) == 1);
  }
  const auto record = nlohmann::json::parse(pigpen::agent::to_json_line(
      pigpen::agent::EpisodeRecord{.summary = summary, .config = config}));
  CHECK(record.at("config") == header);
  CHECK(header.at("scenario").at("opaque_look") == true);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("a log survives text that is not UTF-8") {
  const auto directory = test_directory();
  pigpen::agent::Config config;
  config.model = "pig\xff";
  auto created =
      pigpen::agent::MetricsWriter::create(directory, config, "variant\xc3");
  REQUIRE(created.has_value());
  const auto path = (*created)->path();
  REQUIRE((*created)->record_turn({
      .record = {.turn = 1, .assistant_text = "said \xff"},
  }));
  created->reset();

  const auto records = read_records(path);
  REQUIRE(records.size() == 3);
  CHECK(records[0].at("model") == "pig\xef\xbf\xbd");
  CHECK(records[0].at("prompt_variant") == "variant\xef\xbf\xbd");
  CHECK(records[1].at("assistant_text") == "said \xef\xbf\xbd");
  CHECK(records[2].at("finish_reason") == "abandoned");

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}
