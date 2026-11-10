/// @file gui_options_tests.cpp
/// @brief Covers GUI startup argument parsing: the llamad socket in both
/// value syntaxes, --play, --help, and rejection of empty, unknown, or
/// positional input.

#include "ui/gui_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

TEST_CASE("GUI options populate startup configuration and reject bad input") {
  using namespace std::string_view_literals;

  const std::array<std::string_view, 0> no_arguments{};
  const auto parsed_defaults = pigpen::ui::parse_gui_options(no_arguments);
  REQUIRE(parsed_defaults.has_value());
  CHECK(parsed_defaults->config.socket_path ==
        pigpen::core::default_socket_path());
  CHECK_FALSE(parsed_defaults->play);
  CHECK_FALSE(parsed_defaults->help);

  const std::array separate{"--socket"sv, "/run/user/1000/pig.sock"sv,
                            "--play"sv};
  const auto parsed_separate = pigpen::ui::parse_gui_options(separate);
  REQUIRE(parsed_separate.has_value());
  CHECK(parsed_separate->config.socket_path == "/run/user/1000/pig.sock");
  CHECK(parsed_separate->play);
  CHECK_FALSE(parsed_separate->help);

  const std::array inline_values{"--socket=/tmp/llamad-test.sock"sv};
  const auto parsed_inline = pigpen::ui::parse_gui_options(inline_values);
  REQUIRE(parsed_inline.has_value());
  CHECK(parsed_inline->config.socket_path == "/tmp/llamad-test.sock");
  CHECK_FALSE(parsed_inline->play);

  const std::array help{"--help"sv};
  const auto parsed_help = pigpen::ui::parse_gui_options(help);
  REQUIRE(parsed_help.has_value());
  CHECK(parsed_help->help);

  const std::array missing_socket{"--socket"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(missing_socket));
  const std::array empty_socket{"--socket="sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(empty_socket));
  const std::array empty_separate_socket{"--socket"sv, ""sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(empty_separate_socket));
  const std::array play_value{"--play=yes"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(play_value));
  const std::array help_value{"--help=1"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(help_value));
  const std::array removed_model{"--model"sv, "pig"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(removed_model));
  const std::array unknown{"--unknown"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(unknown));
  const std::array positional{"pig-model"sv};
  CHECK_FALSE(pigpen::ui::parse_gui_options(positional));
}

TEST_CASE("the default socket follows the llamad rule") {
  const char *saved = std::getenv("XDG_RUNTIME_DIR");
  const std::optional<std::string> previous =
      saved == nullptr ? std::nullopt : std::optional<std::string>{saved};

  REQUIRE(::setenv("XDG_RUNTIME_DIR", "/run/user/4242", 1) == 0);
  CHECK(pigpen::core::default_socket_path() == "/run/user/4242/llamad.sock");
  REQUIRE(::setenv("XDG_RUNTIME_DIR", "", 1) == 0);
  const auto fallback = "/tmp/llamad-" + std::to_string(::getuid()) + ".sock";
  CHECK(pigpen::core::default_socket_path() == fallback);
  REQUIRE(::unsetenv("XDG_RUNTIME_DIR") == 0);
  CHECK(pigpen::core::default_socket_path() == fallback);

  if (previous) {
    REQUIRE(::setenv("XDG_RUNTIME_DIR", previous->c_str(), 1) == 0);
  }
}
