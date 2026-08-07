#include "agent/metrics_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::vector<nlohmann::json>
read_records(const std::filesystem::path &path) {
  std::ifstream stream{path};
  std::vector<nlohmann::json> records;
  for (std::string line; std::getline(stream, line);) {
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

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

  const pigpen::agent::WorldEvent event{
      .tick = 1,
      .turn = 1,
      .tool = "eat",
      .arguments = nlohmann::json::object(),
      .result = {{"ok", true}, {"ate", "berry"}, {"reward", 1}, {"score", 1}},
      .before = {.x = 4, .y = 4},
      .after = {.x = 4, .y = 4},
  };
  REQUIRE(writer->record_tool(event, 1).has_value());
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
  REQUIRE(records[1].at("type") == "tool");
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
