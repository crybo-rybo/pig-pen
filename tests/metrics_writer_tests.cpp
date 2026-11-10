/// @file metrics_writer_tests.cpp
/// @brief Covers JSONL header/tool/turn/footer reconciliation, the incomplete
/// "abandoned" footer emitted on destruction, and footer finality.

#include "agent/metrics_writer.hpp"

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

} // namespace

TEST_CASE("metrics log contains a reconcilable header tool turn and footer") {
  const auto directory = test_directory();
  pigpen::core::Config config;
  config.seed = 42;
  config.turn_budget = 2;
  config.temperature = 0.5;
  config.sampling_seed = 7;
  config.socket_path = "/run/user/1000/llamad.sock";
  const pigpen::core::ModelInfo model{.description = "qwen2 0.5B Q4_K - Medium",
                                      .n_params = 494'000'000,
                                      .size_bytes = 398'000'000,
                                      .n_ctx = 8192,
                                      .n_ctx_train = 32768};
  auto created =
      pigpen::agent::MetricsWriter::create(directory, config, model, "default");
  REQUIRE(created.has_value());
  auto writer = std::move(*created);
  const auto path = writer->path();

  const pigpen::core::ToolActivity activity{
      .tick = 1,
      .turn = 1,
      .kind = pigpen::core::ToolKind::eat,
      .outcome = pigpen::core::ToolOutcome::succeeded,
      .arguments_json = "{}",
      .result_json =
          R"({"ate":"berry","ok":true,"reason":null,"reward":1,"score":1})",
      .before = {.x = 4, .y = 4},
      .after = {.x = 4, .y = 4},
      .eaten = pigpen::world::ItemType::berry,
      .score_after = 1,
      .call_id = "eat-1",
      .round = 2,
      .index = 1,
  };
  REQUIRE(writer->record_tool(activity).has_value());
  REQUIRE(writer
              ->record_turn({
                  .turn = 1,
                  .status = pigpen::core::TurnStatus::completed,
                  .assistant_text = "ate a berry",
                  .input_tokens = 900,
                  .output_tokens = 30,
                  .cached_input_tokens = 640,
                  .requests = 3,
                  .tool_calls = 1,
                  .latency = std::chrono::milliseconds{12},
                  .tool_stats =
                      pigpen::core::TurnToolStats{.rounds = 2,
                                                  .calls = 4,
                                                  .rejected_calls = 1,
                                                  .invalid_calls = 1,
                                                  .round_limit_reached = true,
                                                  .unexecuted_calls = 2},
              })
              .has_value());
  REQUIRE(writer
              ->finish(
                  {
                      .reason = pigpen::core::FinishReason::turn_budget,
                      .turns_used = 1,
                  },
                  1)
              .has_value());

  const auto records = read_records(path);
  REQUIRE(records.size() == 4);
  REQUIRE(records.front().at("type") == "header");
  CHECK(records.front().at("format") == 2);
  CHECK(records.front().at("model") == "qwen2 0.5B Q4_K - Medium");
  CHECK(records.front().at("socket") == "/run/user/1000/llamad.sock");
  CHECK(records.front().at("model_info") ==
        nlohmann::json{{"n_ctx", 8192},
                       {"n_ctx_train", 32768},
                       {"n_params", 494'000'000},
                       {"size_bytes", 398'000'000}});
  CHECK_FALSE(records.front().contains("base_url"));
  CHECK(path.filename().string().find("-qwen2_0.5B_Q4_K_-_Medium-42.jsonl") !=
        std::string::npos);
  REQUIRE(records.front().at("seed") == 42);
  REQUIRE(records.front().at("temperature") == 0.5);
  REQUIRE(records.front().at("sampling_seed") == 7);
  REQUIRE(records.front().at("max_output_tokens") == config.max_output_tokens);
  REQUIRE(records.front().at("scenario").at("max_world_tool_calls_per_turn") ==
          4);
  REQUIRE(records[1].at("type") == "tool");
  REQUIRE(records[1].at("args") == nlohmann::json::object());
  CHECK_FALSE(records[1].contains("action_executed"));
  CHECK_FALSE(records[1].contains("result_dispatched"));
  CHECK(records[1].at("call_id") == "eat-1");
  CHECK(records[1].at("round") == 2);
  CHECK(records[1].at("index") == 1);
  REQUIRE(records[1].at("result") ==
          nlohmann::json::parse(activity.result_json));
  REQUIRE(records[2].at("type") == "turn");
  REQUIRE(records[2].at("tool_calls") == 1);
  REQUIRE(records[2].at("zero_tool_turn") == false);
  CHECK(records[2].at("requests") == 3);
  CHECK(records[2].at("input_tokens") == 900);
  CHECK(records[2].at("output_tokens") == 30);
  CHECK(records[2].at("cached_input_tokens") == 640);
  CHECK(records[2].at("tool_stats") ==
        nlohmann::json{{"rounds", 2},
                       {"calls", 4},
                       {"rejected_calls", 1},
                       {"invalid_calls", 1},
                       {"round_limit_reached", true},
                       {"unexecuted_calls", 2}});
  REQUIRE(records.back().at("type") == "footer");
  REQUIRE(records.back().at("complete") == true);
  REQUIRE(records.back().at("final_score") == 1);
  REQUIRE(records.back().at("items_eaten").at("berry") == 1);
  REQUIRE(records.back().at("tool_call_counts").at("eat") == 1);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("destroying an unfinished writer still emits an incomplete footer") {
  const auto directory = test_directory();
  auto created = pigpen::agent::MetricsWriter::create(
      directory, pigpen::core::Config{}, pigpen::core::ModelInfo{}, "default");
  REQUIRE(created.has_value());
  const auto path = (*created)->path();
  REQUIRE((*created)
              ->record_turn({
                  .turn = 3,
                  .status = pigpen::core::TurnStatus::completed,
                  .assistant_text = "partial run",
              })
              .has_value());
  created->reset();

  const auto records = read_records(path);
  REQUIRE(records.size() == 3);
  REQUIRE(records.front().at("sampling_seed").is_null());
  REQUIRE(records.back().at("type") == "footer");
  REQUIRE(records.back().at("complete") == false);
  REQUIRE(records.back().at("finish_reason") == "abandoned");
  REQUIRE(records.back().at("turns_used") == 3);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("metrics log rejects a tool payload that is not JSON") {
  const auto directory = test_directory();
  auto created = pigpen::agent::MetricsWriter::create(
      directory, pigpen::core::Config{}, pigpen::core::ModelInfo{},
      "invalid-payload-test");
  REQUIRE(created.has_value());
  auto writer = std::move(*created);
  const auto path = writer->path();

  CHECK_FALSE(writer->record_tool({
      .kind = pigpen::core::ToolKind::eat,
      .arguments_json = "{not json",
      .eaten = pigpen::world::ItemType::berry,
  }));
  REQUIRE(writer->finish({.reason = pigpen::core::FinishReason::stopped}, 0)
              .has_value());

  const auto records = read_records(path);
  REQUIRE(records.size() == 2);
  CHECK(records.back().at("tool_call_counts").at("eat") == 0);
  CHECK(records.back().at("items_eaten").at("berry") == 0);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("metrics footer is final and cannot be duplicated") {
  const auto directory = test_directory();
  auto created = pigpen::agent::MetricsWriter::create(
      directory, pigpen::core::Config{}, pigpen::core::ModelInfo{},
      "finalization-test");
  REQUIRE(created.has_value());
  auto writer = std::move(*created);
  const auto path = writer->path();

  REQUIRE(writer
              ->finish(
                  {
                      .reason = pigpen::core::FinishReason::stopped,
                      .turns_used = 0,
                  },
                  0)
              .has_value());
  CHECK_FALSE(writer->finish(
      {
          .reason = pigpen::core::FinishReason::stopped,
          .turns_used = 0,
      },
      0));
  CHECK_FALSE(writer->record_turn({.turn = 1}));

  const auto records = read_records(path);
  REQUIRE(records.size() == 2);
  CHECK(records.back().at("type") == "footer");
  CHECK(records.back().at("finish_reason") == "stopped");

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}
