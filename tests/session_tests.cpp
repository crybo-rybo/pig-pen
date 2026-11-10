/// @file session_tests.cpp
/// @brief Covers config rejection, an unreachable daemon, and whole episodes
/// through the real gRPC path against the scripted fake llamad, with the log
/// checked end to end.
///
/// Session is the reset unit: creation either yields the whole composed
/// object or fails without side effects (no directory, no log file).

#include "agent/session.hpp"

#include "support/fake_llamad.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using pigpen::testing::FakeLlamad;
using pigpen::testing::FakeReply;

namespace {

using namespace std::chrono_literals;

/// @brief Yields a unique temp-directory path per invocation so concurrent
/// test runs never share a log directory.
[[nodiscard]] std::filesystem::path session_test_directory() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("pigpen-session-tests-" + std::to_string(stamp));
}

[[nodiscard]] std::vector<nlohmann::json>
read_records(const std::filesystem::path &path) {
  std::ifstream stream{path};
  std::vector<nlohmann::json> records;
  for (std::string line; std::getline(stream, line);) {
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

void pump_until_finished(pigpen::agent::Session &session) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (session.runner().snapshot().state !=
             pigpen::core::RunState::finished &&
         std::chrono::steady_clock::now() < deadline) {
    const auto stats = session.pump();
    if (stats.callbacks_delivered == 0) {
      std::this_thread::sleep_for(1ms);
    }
  }
  REQUIRE(session.runner().snapshot().state ==
          pigpen::core::RunState::finished);
}

struct Daemon {
  explicit Daemon(pigpen::testing::FakeModelInfo info = {})
      : fake(FakeLlamad::start(directory.socket(), std::move(info))) {
    REQUIRE(fake);
  }

  [[nodiscard]] pigpen::core::Config config() const {
    pigpen::core::Config result;
    result.socket_path = directory.socket();
    return result;
  }

  pigpen::testing::TempSocketDirectory directory;
  std::unique_ptr<FakeLlamad> fake;
};

} // namespace

TEST_CASE("session rejects unsafe or incomplete runtime configuration") {
  const auto directory = session_test_directory();
  Daemon daemon;
  auto config = daemon.config();

  config.socket_path.clear();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.socket_path = daemon.directory.socket();

  config.turn_budget = 0;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.turn_budget = 10'001;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.turn_budget = 20;

  config.max_tool_rounds = 0;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.max_tool_rounds = 65;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));

  config.max_tool_rounds = 8;
  config.max_output_tokens = 0;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.max_output_tokens = 2'147'483'648U;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));

  config.max_output_tokens = 2'048;
  config.temperature = -0.1;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.temperature = 2.1;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.temperature = std::numeric_limits<double>::infinity();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.temperature = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  CHECK_FALSE(std::filesystem::exists(directory));
  CHECK(daemon.fake->requests().empty());
}

TEST_CASE("an unreachable daemon is a clear error and leaves no log") {
  const auto directory = session_test_directory();
  pigpen::testing::TempSocketDirectory sockets;
  pigpen::core::Config config;
  config.socket_path = sockets.socket();
  const auto created = pigpen::agent::Session::create(config, directory);
  REQUIRE_FALSE(created);
  CHECK(created.error().starts_with("could not reach llamad at " +
                                    sockets.socket()));
  CHECK_FALSE(std::filesystem::exists(directory));

  Daemon embedding{{.serves_embeddings = true}};
  config.socket_path = embedding.directory.socket();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  CHECK_FALSE(std::filesystem::exists(directory));
}

TEST_CASE("session atomically owns a seeded world, a connection, and a "
          "truthful log") {
  const auto directory = session_test_directory();
  Daemon daemon{{.description = "acme/pig model:Q4_K_M",
                 .n_params = 7,
                 .size_bytes = 8,
                 .n_ctx = 16'384,
                 .n_ctx_train = 32'768}};
  auto config = daemon.config();
  config.seed = 2026;
  config.temperature = 0.2;
  std::filesystem::path log_path;
  {
    auto created =
        pigpen::agent::Session::create(config, directory, "test-preset");
    REQUIRE(created.has_value());
    const auto &session = *created;
    CHECK(session->config() == config);
    CHECK(session->model_info().description == "acme/pig model:Q4_K_M");
    CHECK(session->model_info().n_ctx == 16'384);
    CHECK(session->world().seed() == 2026);
    CHECK(session->runner().snapshot().state == pigpen::core::RunState::idle);
    CHECK(session->tool_activities().empty());
    CHECK(session->metrics_error().empty());
    log_path = session->metrics_path();
    CHECK(std::filesystem::exists(log_path));
    CHECK(log_path.filename().string().find("-acme_pig_model_Q4_K_M-2026") !=
          std::string::npos);
  }

  const auto records = read_records(log_path);
  REQUIRE(records.size() == 2);
  const auto &header = records.front();
  const auto &footer = records.back();
  CHECK(header.at("type") == "header");
  CHECK(header.at("format") == 2);
  CHECK(header.at("prompt_variant") == "test-preset");
  CHECK(header.at("model") == "acme/pig model:Q4_K_M");
  CHECK(header.at("socket") == daemon.directory.socket());
  CHECK(header.at("model_info") == nlohmann::json{{"n_ctx", 16'384},
                                                  {"n_ctx_train", 32'768},
                                                  {"n_params", 7},
                                                  {"size_bytes", 8}});
  CHECK_FALSE(header.contains("base_url"));
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

TEST_CASE("a scripted episode runs through llamad and logs every step") {
  const auto directory = session_test_directory();
  Daemon daemon;
  daemon.fake->enqueue(
      FakeReply::calls({{.id = "c-move",
                         .name = "move",
                         .arguments_json = R"({"direction":"east"})"},
                        {.id = "c-bad",
                         .name = "move",
                         .arguments_json = R"({"direction":"up"})"}},
                       {"Moving east. "}));
  daemon.fake->enqueue({.text = {"Moved."},
                        .prompt_tokens = 300,
                        .completion_tokens = 4,
                        .cached_prompt_tokens = 250});
  daemon.fake->enqueue(
      FakeReply::calls({{.id = "c-look",
                         .name = "look",
                         .arguments_json = R"({ "direction" : "north" })"}}));
  daemon.fake->enqueue(FakeReply::say("Looked."));

  auto config = daemon.config();
  config.turn_budget = 2;
  config.sampling_seed = 77;
  std::filesystem::path log_path;
  {
    auto created = pigpen::agent::Session::create(config, directory);
    REQUIRE(created.has_value());
    auto &session = **created;
    log_path = session.metrics_path();
    REQUIRE(session.play());
    pump_until_finished(session);
    CHECK(session.runner().snapshot().finish_reason ==
          pigpen::core::FinishReason::turn_budget);
    REQUIRE(session.tool_activities().size() == 2);
    CHECK(session.world().position() == (pigpen::world::Position{6, 5}));
    CHECK(session.token_totals().input == 300);
    CHECK(session.token_totals().cached_input == 250);
  }

  const auto requests = daemon.fake->requests();
  REQUIRE(requests.size() == 4);
  for (const auto &request : requests) {
    CHECK(request.seed == 77U);
    REQUIRE(request.tools.size() == 3);
    CHECK(request.messages.front().role == "system");
  }
  // Turn 2 resends turn 1, with both answers and the call ids.
  const auto &resent = requests[2].messages;
  REQUIRE(resent.size() == 7);
  CHECK(resent[2].role == "assistant");
  CHECK(resent[2].content == "Moving east. ");
  CHECK(resent[2].tool_calls.size() == 2);
  CHECK(resent[3].tool_call_id == "c-move");
  CHECK(resent[4].tool_call_id == "c-bad");
  CHECK(nlohmann::json::parse(resent[4].content) ==
        nlohmann::json{{"error", "$.direction is not a declared enumerator; "
                                 "must be one of: north, south, east, west"}});
  CHECK(resent[5].content == "Moved.");
  CHECK(resent[6].role == "user");

  const auto records = read_records(log_path);
  std::vector<nlohmann::json> tools;
  std::vector<nlohmann::json> turns;
  for (const auto &record : records) {
    if (record.at("type") == "tool") {
      tools.push_back(record);
    } else if (record.at("type") == "turn") {
      turns.push_back(record);
    }
  }
  REQUIRE(tools.size() == 2);
  CHECK(tools[0].at("call_id") == "c-move");
  CHECK(tools[0].at("round") == 1);
  CHECK(tools[0].at("index") == 0);
  CHECK(tools[0].at("turn") == 1);
  CHECK(tools[0].at("tick") == 1);
  CHECK(tools[0].at("args") == nlohmann::json{{"direction", "east"}});
  CHECK(tools[0].at("result") == nlohmann::json::parse(resent[3].content));
  CHECK_FALSE(tools[0].contains("result_dispatched"));
  CHECK_FALSE(tools[0].contains("action_executed"));
  // The canonical arguments, not the model's spelling.
  CHECK(tools[1].at("args") == nlohmann::json{{"direction", "north"}});
  CHECK(tools[1].at("turn") == 2);
  CHECK(tools[1].at("result") ==
        nlohmann::json::parse(requests[3].messages.at(8).content));

  REQUIRE(turns.size() == 2);
  CHECK(turns[0].at("status") == "completed");
  CHECK(turns[0].at("assistant_text") == "Moving east. Moved.");
  CHECK(turns[0].at("requests") == 2);
  CHECK(turns[0].at("tool_calls") == 1);
  CHECK(turns[0].at("input_tokens") == 300);
  CHECK(turns[0].at("cached_input_tokens") == 250);
  CHECK(turns[0].at("tool_stats") ==
        nlohmann::json{{"rounds", 1},
                       {"calls", 2},
                       {"rejected_calls", 0},
                       {"invalid_calls", 1},
                       {"round_limit_reached", false},
                       {"unexecuted_calls", 0}});
  CHECK(turns[1].at("requests") == 2);
  const auto &footer = records.back();
  CHECK(footer.at("type") == "footer");
  CHECK(footer.at("complete") == true);
  CHECK(footer.at("finish_reason") == "turn_budget");
  CHECK(footer.at("tool_call_counts") ==
        nlohmann::json{{"eat", 0}, {"look", 1}, {"move", 1}});

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("stop cancels a hanging request and still writes the footer") {
  const auto directory = session_test_directory();
  Daemon daemon;
  daemon.fake->enqueue({.text = {"Thinking"}, .hang = true});
  std::filesystem::path log_path;
  {
    auto created = pigpen::agent::Session::create(daemon.config(), directory);
    REQUIRE(created.has_value());
    auto &session = **created;
    log_path = session.metrics_path();
    REQUIRE(session.play());
    static_cast<void>(session.pump());
    REQUIRE(daemon.fake->wait_for_requests(1));
    REQUIRE(session.stop());
    pump_until_finished(session);
    CHECK(session.runner().snapshot().finish_reason ==
          pigpen::core::FinishReason::stopped);
  }
  const auto records = read_records(log_path);
  REQUIRE(records.size() == 3);
  CHECK(records[1].at("type") == "turn");
  CHECK(records[1].at("status") == "cancelled");
  CHECK(records[1].at("requests") == 1);
  CHECK(records.back().at("finish_reason") == "stopped");
  CHECK(records.back().at("complete") == true);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("destroying a session with a request in flight writes an "
          "abandoned footer") {
  const auto directory = session_test_directory();
  Daemon daemon;
  daemon.fake->enqueue({.hang = true});
  std::filesystem::path log_path;
  {
    auto created = pigpen::agent::Session::create(daemon.config(), directory);
    REQUIRE(created.has_value());
    log_path = (*created)->metrics_path();
    REQUIRE((*created)->play());
    static_cast<void>((*created)->pump());
    REQUIRE(daemon.fake->wait_for_requests(1));
  }
  CHECK(daemon.fake->wait_until_idle());
  const auto records = read_records(log_path);
  REQUIRE(records.size() == 2);
  CHECK(records.back().at("finish_reason") == "abandoned");

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}
