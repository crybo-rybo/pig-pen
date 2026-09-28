/// @file gui_options_tests.cpp
/// @brief Covers GUI startup argument parsing: model and endpoint in both
/// value syntaxes, reward overrides, --help, and the GUI's refusal of an
/// empty model or endpoint. The shared parser itself is covered by
/// cli_options_tests.cpp.

#include "ui/gui_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

TEST_CASE("GUI options populate startup configuration and reject bad input") {
  using namespace std::string_view_literals;

  const std::array<std::string_view, 0> no_arguments{};
  const auto parsed_defaults = pigpen::ui::parse_gui_options(no_arguments);
  REQUIRE(parsed_defaults.has_value());
  CHECK(parsed_defaults->config.model.empty());
  CHECK(parsed_defaults->config.base_url == "http://127.0.0.1:11434/v1");
  CHECK(parsed_defaults->reward_weights == pigpen::agent::RewardWeights{});

  const std::array separate{"--model"sv, "registry.example/pig-model:Q4_K_M"sv,
                            "--base-url"sv, "http://model-host.test/v1"sv};
  const auto parsed_separate = pigpen::ui::parse_gui_options(separate);
  REQUIRE(parsed_separate.has_value());
  CHECK(parsed_separate->config.model == "registry.example/pig-model:Q4_K_M");
  CHECK(parsed_separate->config.base_url == "http://model-host.test/v1");
  CHECK_FALSE(parsed_separate->help);

  const std::array inline_values{
      "--model=another/model:latest"sv,
      "--base-url=http://127.0.0.1:9999/v1"sv,
  };
  const auto parsed_inline = pigpen::ui::parse_gui_options(inline_values);
  REQUIRE(parsed_inline.has_value());
  CHECK(parsed_inline->config.model == "another/model:latest");
  CHECK(parsed_inline->config.base_url == "http://127.0.0.1:9999/v1");

  const std::array help{"--help"sv};
  const auto parsed_help = pigpen::ui::parse_gui_options(help);
  REQUIRE(parsed_help.has_value());
  CHECK(parsed_help->help);
  CHECK(parsed_help->config.model.empty());

  const std::array missing_model{"--model"sv};
  CHECK(pigpen::ui::parse_gui_options(missing_model).error() ==
        "--model requires a value");
  const std::array empty_model{"--model="sv};
  CHECK(pigpen::ui::parse_gui_options(empty_model).error() ==
        "--model requires a value");
  const std::array empty_separate_model{"--model"sv, ""sv};
  CHECK(pigpen::ui::parse_gui_options(empty_separate_model).error() ==
        "--model requires a value");
  const std::array empty_base_url{"--base-url"sv, ""sv};
  CHECK(pigpen::ui::parse_gui_options(empty_base_url).error() ==
        "--base-url requires a value");
  const std::array help_value{"--help=yes"sv};
  CHECK(pigpen::ui::parse_gui_options(help_value).error() ==
        "--help does not take a value");
  const std::array unknown{"--unknown"sv};
  CHECK(pigpen::ui::parse_gui_options(unknown).error() ==
        "unknown option: --unknown");
  const std::array positional{"pig-model"sv};
  CHECK(pigpen::ui::parse_gui_options(positional).error() ==
        "unexpected positional argument: pig-model");
  // Both values reach the log header, which must be valid JSON.
  const std::array bad_model{"--model"sv, "pig\xff"sv};
  CHECK(pigpen::ui::parse_gui_options(bad_model).error() ==
        "--model must be valid UTF-8");
  const std::array bad_base_url{"--base-url=http://h\xc3/v1"sv};
  CHECK(pigpen::ui::parse_gui_options(bad_base_url).error() ==
        "--base-url must be valid UTF-8");
  // The episode flags belong to the Controls panel, not the command line.
  const std::array turns{"--turns"sv, "4"sv};
  CHECK(pigpen::ui::parse_gui_options(turns).error() ==
        "unknown option: --turns");
}

TEST_CASE("GUI options accept reward weight overrides") {
  using namespace std::string_view_literals;

  const std::array rewards{"--reward"sv, "invalid_call=-2"sv,
                           "--reward=explored_cell=0.1"sv};
  const auto parsed = pigpen::ui::parse_gui_options(rewards);
  REQUIRE(parsed.has_value());
  CHECK(parsed->reward_weights.invalid_call == -2.0);
  CHECK(parsed->reward_weights.explored_cell == 0.1);

  const std::array bogus{"--reward"sv, "bogus=1"sv};
  CHECK(pigpen::ui::parse_gui_options(bogus).error().starts_with(
      "unknown reward weight \"bogus\""));
}

TEST_CASE("GUI help lists its options") {
  const auto help = pigpen::ui::gui_options_help();
  CHECK(help.contains("  --model NAME"));
  CHECK(help.contains("  --base-url URL"));
  CHECK(help.contains("(default: http://127.0.0.1:11434/v1)"));
  CHECK(help.contains("  --reward NAME=VALUE"));
  CHECK(help.contains("  --help"));
}
