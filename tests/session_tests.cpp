/// @file session_tests.cpp
/// @brief Covers config and request-header rejection, that a session
/// atomically owns a seeded world, a registered tool harness, and an
/// already-open truthful log (or, without a log directory, none at all), and
/// that its summary and footer carry the reward weights it was created with.
///
/// Session is the reset unit: creation either yields the whole composed
/// object or fails without side effects (no directory, no log file).

#include "agent/session.hpp"

#include "agent/episode_summary.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace {

/// @brief Yields a unique temp-directory path per invocation so concurrent
/// test runs never share a log directory.
[[nodiscard]] std::filesystem::path session_test_directory() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("pigpen-session-tests-" + std::to_string(stamp));
}

} // namespace

TEST_CASE("session rejects unsafe or incomplete runtime configuration") {
  const auto directory = session_test_directory();
  pigpen::agent::Config config;
  config.model = "registry.example/pig-model:Q4_K_M";

  config.base_url = "localhost:11434/v1";
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.base_url.clear();
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.base_url = "http://127.0.0.1:11434/v1";

  config.model.clear();
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.model = "registry.example/pig-model:Q4_K_M";

  config.turn_budget = 0;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.turn_budget = 10'001;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.turn_budget = 20;

  config.max_tool_rounds = 0;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.max_tool_rounds = 65;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));

  config.max_tool_rounds = 8;
  config.max_output_tokens = 0;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));

  config.max_output_tokens = 2'048;
  config.temperature = -0.1;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.temperature = 2.1;
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.temperature = std::numeric_limits<double>::infinity();
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  config.temperature = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(
      pigpen::agent::Session::create(config, {.log_directory = directory}));
  CHECK_FALSE(std::filesystem::exists(directory));
}

TEST_CASE("session atomically owns a seeded world registered harness and "
          "truthful log") {
  const auto directory = session_test_directory();
  pigpen::agent::Config config;
  config.model = "registry.example/pig-model:Q4_K_M";
  config.seed = 2026;
  config.temperature = 0.2;
  std::filesystem::path log_path;
  {
    auto created =
        pigpen::agent::Session::create(config, {.log_directory = directory,
                                                .prompt_variant = "test-preset",
                                                .rollout_id = "run42/2026/1"});
    REQUIRE(created.has_value());
    const auto &session = *created;
    CHECK(session->config() == config);
    CHECK(session->world().seed() == 2026);
    CHECK(session->runner().snapshot().state == pigpen::agent::RunState::idle);
    CHECK(session->tool_activities().empty());
    CHECK(session->turns().empty());
    const auto first = session->elapsed();
    CHECK(first >= std::chrono::milliseconds::zero());
    CHECK(session->elapsed() >= first);
    CHECK(session->metrics_error().empty());
    REQUIRE(session->metrics_path());
    log_path = *session->metrics_path();
    CHECK(std::filesystem::exists(log_path));
  }

  std::ifstream stream{log_path};
  std::string header_line;
  std::string footer_line;
  REQUIRE(std::getline(stream, header_line));
  REQUIRE(std::getline(stream, footer_line));
  const auto header = nlohmann::json::parse(header_line);
  const auto footer = nlohmann::json::parse(footer_line);
  CHECK(header.at("prompt_variant") == "test-preset");
  CHECK(header.at("rollout_id") == "run42/2026/1");
  CHECK(header.at("model") == config.model);
  CHECK(header.at("seed") == 2026);
  CHECK(header.at("temperature") == 0.2);
  CHECK(header.at("max_output_tokens") == config.max_output_tokens);
  CHECK(header.at("scenario").at("max_world_tool_calls_per_turn") == 4);
  CHECK(footer.at("type") == "footer");
  CHECK(footer.at("finish_reason") == "abandoned");
  CHECK(footer.at("complete") == false);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("a session summarises itself with its own reward weights") {
  const auto directory = session_test_directory();
  pigpen::agent::Config config;
  config.model = "registry.example/pig-model:Q4_K_M";
  pigpen::agent::RewardWeights weights;
  weights.zero_tool_turn = -3.0;
  std::filesystem::path log_path;
  {
    auto created = pigpen::agent::Session::create(
        config, {.log_directory = directory, .reward_weights = weights});
    REQUIRE(created.has_value());
    const auto &session = *created;
    CHECK(session->reward_weights() == weights);

    const auto live = pigpen::agent::summarize_episode(*session, weights);
    CHECK_FALSE(live.complete());
    CHECK_FALSE(live.reward.valid);
    CHECK(live.reward.invalid_reason == "unfinished");
    CHECK(live.reward_weights == weights);

    // Stopping an idle episode finishes it at once and writes the footer.
    REQUIRE(session->stop());
    const auto stopped = pigpen::agent::summarize_episode(*session, weights);
    CHECK(stopped.finish_reason == pigpen::agent::FinishReason::stopped);
    CHECK(stopped.reward.invalid_reason == "stopped");
    // The clock stops with the episode, so the duration stays put.
    const auto duration = session->elapsed();
    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    CHECK(session->elapsed() == duration);
    CHECK(stopped.duration == duration);
    REQUIRE(session->metrics_path());
    log_path = *session->metrics_path();
  }

  std::ifstream stream{log_path};
  std::string line;
  std::string last_line;
  while (std::getline(stream, line)) {
    last_line = line;
  }
  const auto footer = nlohmann::json::parse(last_line);
  CHECK(footer.at("type") == "footer");
  CHECK(footer.at("complete") == true);
  CHECK(footer.at("finish_reason") == "stopped");
  CHECK(footer.at("reward_weights").at("zero_tool_turn") == -3.0);
  CHECK(footer.at("reward").at("valid") == false);
  CHECK(footer.at("reward").at("total").is_null());

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("session rejects bad request headers before opening a log") {
  const auto directory = session_test_directory();
  pigpen::agent::Config config;
  config.model = "registry.example/pig-model:Q4_K_M";
  const auto create = [&](pigpen::agent::SessionOptions options) {
    options.log_directory = directory;
    return pigpen::agent::Session::create(config, std::move(options));
  };

  // Pig Pen manages the rollout header, in any letter case.
  const auto reserved =
      create({.request_headers = {{"x-pigpen-ROLLOUT", "run/1/0"}}});
  REQUIRE_FALSE(reserved);
  CHECK(reserved.error() ==
        "request header x-pigpen-ROLLOUT is reserved for the rollout id");
  // Scry rejects headers it manages and malformed names or values.
  CHECK_FALSE(create({.request_headers = {{"Authorization", "Bearer x"}}}));
  CHECK_FALSE(create({.request_headers = {{"Content-Type", "text/plain"}}}));
  CHECK_FALSE(create({.request_headers = {{"bad name", "value"}}}));
  CHECK_FALSE(create({.request_headers = {{"X-Pigpen-Seed", "1\r\n2"}}}));
  CHECK_FALSE(create({.rollout_id = "run\n1"}));
  CHECK_FALSE(std::filesystem::exists(directory));

  auto accepted = create(
      {.rollout_id = "run42/7/3", .request_headers = {{"X-Pigpen-Seed", "7"}}});
  REQUIRE(accepted);
  accepted->reset();
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("a session without a log directory keeps its facts and writes "
          "nothing") {
  const auto directory = session_test_directory();
  std::filesystem::create_directories(directory);
  // Anything written relative to the working directory would land here.
  // Restored even when a REQUIRE below throws.
  struct WorkingDirectory {
    std::filesystem::path previous{std::filesystem::current_path()};
    ~WorkingDirectory() {
      std::error_code ignored;
      std::filesystem::current_path(previous, ignored);
    }
  };
  std::optional<WorkingDirectory> working_directory{std::in_place};
  std::filesystem::current_path(directory);
  pigpen::agent::Config config;
  config.model = "registry.example/pig-model:Q4_K_M";
  pigpen::agent::RewardWeights weights;
  weights.score = 2.0;
  {
    auto created = pigpen::agent::Session::create(
        config, {.rollout_id = "run42/0/0", .reward_weights = weights});
    REQUIRE(created.has_value());
    const auto &session = *created;
    CHECK_FALSE(session->metrics_path());
    CHECK(session->metrics_error().empty());
    CHECK(session->reward_weights() == weights);
    CHECK_FALSE(session->finished());

    REQUIRE(session->stop());
    CHECK(session->finished());
    CHECK(session->metrics_error().empty());
    const auto summary = pigpen::agent::summarize_episode(*session, weights);
    CHECK(summary.finish_reason == pigpen::agent::FinishReason::stopped);
    CHECK(summary.complete());
    CHECK(summary.reward.invalid_reason == "stopped");
    CHECK(summary.reward_weights == weights);
  }
  working_directory.reset();
  CHECK(std::filesystem::is_empty(directory));

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}
