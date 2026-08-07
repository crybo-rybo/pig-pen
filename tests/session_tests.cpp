#include "agent/session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace {

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

  config.base_url.clear();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.base_url = "http://127.0.0.1:11434/v1";

  config.model.clear();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.model = "registry.example/pig-model:Q4_K_M";

  config.turn_budget = 0;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.turn_budget = 20;

  config.max_tool_rounds = 0;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.max_tool_rounds = 65;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));

  config.max_tool_rounds = 8;
  config.temperature = -0.1;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.temperature = 2.1;
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.temperature = std::numeric_limits<double>::infinity();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
  config.temperature = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(pigpen::agent::Session::create(config, directory));
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
        pigpen::agent::Session::create(config, directory, "test-preset");
    REQUIRE(created.has_value());
    const auto &session = *created;
    CHECK(session->config() == config);
    CHECK(session->world().seed() == 2026);
    CHECK(session->runner().snapshot().state == pigpen::agent::RunState::idle);
    CHECK(session->events().empty());
    CHECK(session->metrics_error().empty());
    log_path = session->metrics_path();
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
  CHECK(header.at("model") == config.model);
  CHECK(header.at("seed") == 2026);
  CHECK(header.at("temperature") == 0.2);
  CHECK(footer.at("type") == "footer");
  CHECK(footer.at("finish_reason") == "abandoned");
  CHECK(footer.at("complete") == false);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}
