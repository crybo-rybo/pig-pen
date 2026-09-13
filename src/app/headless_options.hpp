/// @file headless_options.hpp
/// @brief Testable command-line parsing for the headless entry point.
#pragma once

#include "agent/config.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pigpen::app {

/// @brief The full parsed headless command line.
struct HeadlessOptions {
  agent::Config config{};
  std::filesystem::path log_directory{"logs"};
  std::chrono::seconds timeout{300};
  std::string prompt_variant{"default"};
  std::optional<std::string> user_input{};
  bool help{};
};

/// @brief Parse arguments after argv[0], accepting `--option value` and
/// `--option=value` forms.
/// @return Parsed options, or the diagnostic printed after `option error:`.
/// @note `--help` skips required-field validation but does not hide malformed
/// or unknown arguments.
[[nodiscard]] std::expected<HeadlessOptions, std::string>
parse_headless_options(std::span<const std::string_view> arguments);

} // namespace pigpen::app
