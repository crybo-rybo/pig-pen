/// @file metrics_writer_tests.cpp
/// @brief Covers JSONL header/tool/turn/footer reconciliation, the incomplete
/// "abandoned" footer emitted on destruction, and footer finality.

#include "agent/metrics_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

/// @brief Owns a uniquely created temporary directory and removes it even when
/// an assertion aborts its test case.
class TestDirectory final {
public:
  TestDirectory() {
    const auto base =
        std::filesystem::temp_directory_path() /
        ("pigpen-metrics-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    for (std::size_t suffix = 0;; ++suffix) {
      path_ = suffix == 0 ? base
                          : std::filesystem::path{base.string() + '-' +
                                                  std::to_string(suffix)};
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
      if (error && error != std::errc::file_exists) {
        throw std::runtime_error{"could not create metrics test directory: " +
                                 error.message()};
      }
    }
  }

  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TestDirectory(const TestDirectory &) = delete;
  TestDirectory &operator=(const TestDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_{};
};

/// @brief Reads a flushed log byte-for-byte for overwrite checks.
[[nodiscard]] std::string read_text(const std::filesystem::path &path) {
  std::ifstream stream{path};
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("metrics log contains a reconcilable header tool turn and footer") {
  const TestDirectory directory;
  pigpen::agent::Config config;
  config.seed = 42;
  config.turn_budget = 2;
  config.temperature = 0.5;
  auto created =
      pigpen::agent::MetricsWriter::create(directory.path(), config, "default");
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
          R"({"action_executed":true,"ate":"berry","error":null,"error_code":null,"ok":true,"reason":null,"reward":1,"score":1,"turn_tool_budget":{"instruction":"3 world-tool calls remain in this turn.","remaining":3,"used":1}})",
      .before = {.x = 4, .y = 4},
      .after = {.x = 4, .y = 4},
      .eaten = pigpen::world::ItemType::berry,
      .score_after = 1,
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
  REQUIRE(records[1].at("args") == nlohmann::json::object());
  REQUIRE(records[1].at("action_executed") == true);
  REQUIRE(records[1].at("result") ==
          nlohmann::json::parse(activity.result_json));
  REQUIRE(records[2].at("type") == "turn");
  REQUIRE(records[2].at("tool_calls") == 1);
  REQUIRE(records[2].at("zero_tool_turn") == false);
  REQUIRE(records.back().at("type") == "footer");
  REQUIRE(records.back().at("complete") == true);
  REQUIRE(records.back().at("final_score") == 1);
  REQUIRE(records.back().at("items_eaten").at("berry") == 1);
  REQUIRE(records.back().at("tool_call_counts").at("eat") == 1);
}

TEST_CASE("destroying an unfinished writer still emits an incomplete footer") {
  const TestDirectory directory;
  auto created = pigpen::agent::MetricsWriter::create(
      directory.path(), pigpen::agent::Config{}, "default");
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
}

TEST_CASE("metrics footer is final and cannot be duplicated") {
  const TestDirectory directory;
  auto created = pigpen::agent::MetricsWriter::create(
      directory.path(), pigpen::agent::Config{}, "finalization-test");
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
}

TEST_CASE("metrics writer reports non-collision file creation errors") {
  const TestDirectory directory;
  pigpen::agent::Config config;
  config.model = std::string(5'000, 'x');

  const auto created = pigpen::agent::MetricsWriter::create(
      directory.path(), config, "invalid-filename");

  REQUIRE_FALSE(created.has_value());
  CHECK(created.error().starts_with("could not open metrics log: "));
}

TEST_CASE("concurrent metrics writers reserve distinct files without replacing "
          "an existing log") {
  constexpr int writer_count = 32;
  const TestDirectory directory;
  pigpen::agent::Config config;
  config.model = "concurrent-writer-test";
  config.seed = 8675309;

  auto existing = pigpen::agent::MetricsWriter::create(directory.path(), config,
                                                       "existing-log");
  REQUIRE(existing.has_value());
  REQUIRE((*existing)
              ->record_turn({
                  .turn = 1,
                  .status = pigpen::agent::TurnStatus::completed,
                  .assistant_text = "preserve this record",
              })
              .has_value());
  const auto existing_path = (*existing)->path();
  const auto existing_contents = read_text(existing_path);

  struct CreationResult {
    std::unique_ptr<pigpen::agent::MetricsWriter> writer;
    std::string error;
  };
  std::array<CreationResult, writer_count> results;
  std::array<std::thread, writer_count> threads;
  std::barrier start_line{writer_count + 1};

  for (std::size_t index = 0; index < results.size(); ++index) {
    threads[index] = std::thread{[&, index] {
      start_line.arrive_and_wait();
      auto created = pigpen::agent::MetricsWriter::create(
          directory.path(), config, "concurrent-log");
      if (created) {
        results[index].writer = std::move(*created);
      } else {
        results[index].error = std::move(created.error());
      }
    }};
  }
  start_line.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }

  std::set<std::filesystem::path> paths{existing_path};
  for (const auto &result : results) {
    INFO(result.error);
    REQUIRE(result.writer);
    CHECK(paths.insert(result.writer->path()).second);
    const auto records = read_records(result.writer->path());
    REQUIRE(records.size() == 1);
    CHECK(records.front().at("type") == "header");
  }
  CHECK(paths.size() == static_cast<std::size_t>(writer_count + 1));
  CHECK(read_text(existing_path) == existing_contents);
}
