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

  const std::array separate{"--model"sv, "registry.example/pig-model:Q4_K_M"sv,
                            "--base-url"sv, "http://model-host.test/v1"sv};
  const auto parsed_separate = pigpen::ui::parse_gui_options(separate);
  REQUIRE(parsed_separate.has_value());
  CHECK(parsed_separate->config.model ==
        "registry.example/pig-model:Q4_K_M");
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
  CHECK_FALSE(pigpen::ui::parse_gui_options(missing_model));
  const std::array empty_model{"--model="sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(empty_model));
  const std::array empty_separate_model{"--model"sv, ""sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(empty_separate_model));
  const std::array unknown{"--unknown"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(unknown));
  const std::array positional{"pig-model"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(positional));
}
