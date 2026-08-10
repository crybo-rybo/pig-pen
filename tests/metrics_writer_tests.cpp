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
  pigpen::agent::Config config;
  config.seed = 42;
  config.turn_budget = 2;
  config.temperature = 0.5;
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
      .before = {.x = 4, .y = 4},
      .after = {.x = 4, .y = 4},
      .eaten = pigpen::world::ItemType::berry,
      .score_after = 1,
      .summary = "ate berry (+1) at (4, 4)",
  };
  REQUIRE(writer->record_tool(activity).has_value());
  REQUIRE(writer
              ->record_turn({
                  .turn = 1,
                  .status = pigpen::agent::TurnStatus::completed,
                  .assistant_text = "ate a berry",
                  .tool_calls = 1,
                  .latency = std::chrono::milliseconds{12},
              })
              .has_value());
  REQUIRE(writer
              ->finish(
                  {
                      .reason = pigpen::agent::FinishReason::turn_budget,
                      .turns_used = 1,
                  },
                  1)
              .has_value());
  REQUIRE(writer->finalized());

  const auto records = read_records(path);
  REQUIRE(records.size() == 4);
  REQUIRE(records.front().at("type") == "header");
  REQUIRE(records.front().at("seed") == 42);
  REQUIRE(records.front().at("temperature") == 0.5);
  REQUIRE(records.front().at("max_output_tokens") == config.max_output_tokens);
  REQUIRE(records.front().at("scenario").at("max_world_tool_calls_per_turn") ==
          4);
  REQUIRE(records[1].at("type") == "tool");
  REQUIRE(records[1].at("tool") == "eat");
  REQUIRE(records[1].at("outcome") == "succeeded");
  REQUIRE(records[1].at("action_executed") == true);
  REQUIRE(records[1].at("direction").is_null());
  REQUIRE(records[1].at("eaten") == "berry");
  REQUIRE(records[1].at("before") == nlohmann::json({{"x", 4}, {"y", 4}}));
  REQUIRE(records[1].at("score_after") == 1);
  REQUIRE(records[1].at("summary") == activity.summary);
  REQUIRE(records[2].at("type") == "turn");
  REQUIRE(records[2].at("tool_calls") == 1);
  REQUIRE(records[2].at("zero_tool_turn") == false);
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
      directory, pigpen::agent::Config{}, "default");
  REQUIRE(created.has_value());
  const auto path = (*created)->path();
  REQUIRE((*created)
              ->record_turn({
                  .turn = 3,
                  .status = pigpen::agent::TurnStatus::completed,
                  .assistant_text = "partial run",
              })
              .has_value());
  created->reset();

  const auto records = read_records(path);
  REQUIRE(records.size() == 3);
  REQUIRE(records.back().at("type") == "footer");
  REQUIRE(records.back().at("complete") == false);
  REQUIRE(records.back().at("finish_reason") == "abandoned");
  REQUIRE(records.back().at("turns_used") == 3);

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

  REQUIRE(writer
              ->finish(
                  {
                      .reason = pigpen::agent::FinishReason::stopped,
                      .turns_used = 0,
                  },
                  0)
              .has_value());
  CHECK_FALSE(writer->finish(
      {
          .reason = pigpen::agent::FinishReason::stopped,
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
