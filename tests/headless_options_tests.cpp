/// @file headless_options_tests.cpp
/// @brief Covers headless option syntax, runtime boundaries, and diagnostics.

#include "app/headless_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace {

template <std::size_t Size>
[[nodiscard]] auto parse(const std::array<std::string_view, Size> &arguments) {
  return pigpen::app::parse_headless_options(arguments);
}

template <std::size_t Size>
void check_error(const std::array<std::string_view, Size> &arguments,
                 const std::string_view expected) {
  const auto parsed = parse(arguments);
  REQUIRE_FALSE(parsed.has_value());
  CHECK(parsed.error() == expected);
}

} // namespace

TEST_CASE("headless options parse both value syntaxes and scenario flags") {
  using namespace std::string_view_literals;

  const std::array arguments{
      "--base-url"sv,
      "http://model-host.test/v1"sv,
      "--model=registry.example/pig-model:Q4_K_M"sv,
      "--seed"sv,
      "18446744073709551615"sv,
      "--turns=10000"sv,
      "--max-tool-rounds"sv,
      "64"sv,
      "--temperature=2.0"sv,
      "--timeout-seconds"sv,
      "86400"sv,
      "--log-dir=test-logs"sv,
      "--prompt-variant"sv,
      "experiment-a"sv,
      "--input=try the eastern edge"sv,
      "--hidden-values"sv,
      "--no-reward-feedback"sv,
      "--opaque-look"sv,
  };
  const auto parsed = parse(arguments);

  REQUIRE(parsed.has_value());
  CHECK(parsed->config.base_url == "http://model-host.test/v1");
  CHECK(parsed->config.model == "registry.example/pig-model:Q4_K_M");
  CHECK(parsed->config.seed == std::numeric_limits<std::uint64_t>::max());
  CHECK(parsed->config.turn_budget == pigpen::agent::max_turn_budget);
  CHECK(parsed->config.max_tool_rounds == pigpen::agent::max_tool_rounds);
  CHECK(parsed->config.temperature == pigpen::agent::max_temperature);
  CHECK(parsed->timeout == std::chrono::seconds{86'400});
  CHECK(parsed->log_directory == "test-logs");
  CHECK(parsed->prompt_variant == "experiment-a");
  REQUIRE(parsed->user_input.has_value());
  CHECK(*parsed->user_input == "try the eastern edge");
  CHECK_FALSE(parsed->config.known_item_values);
  CHECK_FALSE(parsed->config.reward_feedback);
  CHECK(parsed->config.opaque_look);
  CHECK_FALSE(parsed->help);
}

TEST_CASE("headless options accept lower runtime boundaries") {
  using namespace std::string_view_literals;
  const std::array arguments{
      "--model"sv, "test-model"sv,          "--turns"sv,
      "1"sv,       "--max-tool-rounds=1"sv, "--temperature"sv,
      "0.0"sv,     "--timeout-seconds=1"sv,
  };

  const auto parsed = parse(arguments);
  REQUIRE(parsed.has_value());
  CHECK(parsed->config.turn_budget == pigpen::agent::min_turn_budget);
  CHECK(parsed->config.max_tool_rounds == pigpen::agent::min_tool_rounds);
  CHECK(parsed->config.temperature == pigpen::agent::min_temperature);
  CHECK(parsed->timeout == std::chrono::seconds{1});
}

TEST_CASE("headless help skips required model validation") {
  using namespace std::string_view_literals;
  const std::array arguments{"--help"sv};

  const auto parsed = parse(arguments);
  REQUIRE(parsed.has_value());
  CHECK(parsed->help);
  CHECK(parsed->config.model.empty());
}

TEST_CASE("headless options preserve focused syntax diagnostics") {
  using namespace std::string_view_literals;

  check_error(std::array{"model-name"sv},
              "unexpected positional argument: model-name"sv);
  check_error(std::array{"--unknown"sv}, "unknown option: --unknown"sv);
  check_error(std::array{"--model"sv}, "--model requires a value"sv);
  check_error(std::array{"--model="sv}, "--model requires a value"sv);
  check_error(std::array{"--model"sv, ""sv}, "--model requires a value"sv);
  check_error(std::array{"--model=test"sv, "--input"sv, ""sv},
              "--input requires a value"sv);
  check_error(std::array{"--help"sv, "--model"sv, ""sv},
              "--model requires a value"sv);
  check_error(std::array{"--hidden-values=yes"sv},
              "--hidden-values does not take a value"sv);
  check_error(std::array{"--help=yes"sv}, "--help does not take a value"sv);
  check_error(std::array{"--help"sv, "--unknown"sv},
              "unknown option: --unknown"sv);
  check_error(std::array{"--model=test"sv, "--turns"sv, "--opaque-look"sv},
              "--turns requires a value"sv);
}

TEST_CASE("headless options reject values outside every numeric boundary") {
  using namespace std::string_view_literals;

  check_error(std::array{"--model=test"sv, "--turns=0"sv},
              "--turns must be in the range 1..10000"sv);
  check_error(std::array{"--model=test"sv, "--turns=10001"sv},
              "--turns must be in the range 1..10000"sv);
  check_error(std::array{"--model=test"sv, "--max-tool-rounds=0"sv},
              "--max-tool-rounds must be in the range 1..64"sv);
  check_error(std::array{"--model=test"sv, "--max-tool-rounds=65"sv},
              "--max-tool-rounds must be in the range 1..64"sv);
  check_error(std::array{"--model=test"sv, "--temperature=-0.1"sv},
              "--temperature must be a finite number in the range 0.0..2.0"sv);
  check_error(std::array{"--model=test"sv, "--temperature=nan"sv},
              "--temperature must be a finite number in the range 0.0..2.0"sv);
  check_error(std::array{"--model=test"sv, "--temperature=2.1"sv},
              "--temperature must be a finite number in the range 0.0..2.0"sv);
  check_error(std::array{"--model=test"sv, "--timeout-seconds=0"sv},
              "--timeout-seconds must be in the range 1..86400"sv);
  check_error(std::array{"--model=test"sv, "--timeout-seconds=86401"sv},
              "--timeout-seconds must be in the range 1..86400"sv);
  check_error(std::array{"--model=test"sv, "--seed=-1"sv},
              "--seed requires an unsigned decimal integer"sv);
  check_error(std::array{"--model=test"sv, "--turns=+1"sv},
              "--turns requires an unsigned decimal integer"sv);
  check_error(std::array{"--model=test"sv, "--turns=1x"sv},
              "--turns must be in the range 1..10000"sv);
  check_error(std::array{"--model=test"sv, "--seed=18446744073709551616"sv},
              "--seed must be in the range 0..18446744073709551615"sv);
}

TEST_CASE("config validation reports unsafe direct caller values") {
  pigpen::agent::Config config;
  config.model = "test-model";
  CHECK_FALSE(pigpen::agent::validate_config(config).has_value());

  config.turn_budget = pigpen::agent::max_turn_budget + 1U;
  CHECK(pigpen::agent::validate_config(config) ==
        pigpen::agent::ConfigValidationError::turn_budget_too_large);
  config.turn_budget = pigpen::agent::min_turn_budget;

  config.max_tool_rounds = pigpen::agent::max_tool_rounds + 1U;
  CHECK(pigpen::agent::validate_config(config) ==
        pigpen::agent::ConfigValidationError::tool_rounds_too_large);
  config.max_tool_rounds = pigpen::agent::min_tool_rounds;

  config.max_output_tokens = 0;
  CHECK(pigpen::agent::validate_config(config) ==
        pigpen::agent::ConfigValidationError::output_tokens_too_small);
  config.max_output_tokens = pigpen::agent::min_output_tokens;

  config.temperature = std::numeric_limits<double>::infinity();
  CHECK(pigpen::agent::validate_config(config) ==
        pigpen::agent::ConfigValidationError::temperature_out_of_range);
}
