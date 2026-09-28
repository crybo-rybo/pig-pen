/// @file gui_options.hpp
/// @brief Command-line parsing for the GUI entry point.
#pragma once

#include "agent/config.hpp"
#include "agent/reward.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace pigpen::ui {

/// @brief Result of parsing the GUI command line.
struct GuiOptions {
  /// Config seeded with any `--model` / `--base-url` values; other fields
  /// keep their defaults and are edited in the Controls panel.
  agent::Config config{};
  /// Weights from any `--reward NAME=VALUE` overrides; they apply to every
  /// session the GUI creates.
  agent::RewardWeights reward_weights{};
  /// `--help` was requested; the caller prints usage and exits 0.
  bool help{};
};

/// @brief Parses `--model`, `--base-url`, `--reward`, and `--help` from
/// @p arguments, accepting both `--option value` and `--option=value` forms.
/// `--model` and `--base-url` reject an empty value.
/// @return Parsed options, or a human-readable error for the caller to print.
[[nodiscard]] std::expected<GuiOptions, std::string>
parse_gui_options(std::span<const std::string_view> arguments);

/// @brief The option lines of the GUI's usage text.
[[nodiscard]] std::string gui_options_help();

} // namespace pigpen::ui
