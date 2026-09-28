/// @file gui_options.cpp
/// @brief Implements command-line parsing for the GUI entry point.
#include "ui/gui_options.hpp"

#include "cli/config_options.hpp"
#include "cli/option_parser.hpp"

#include <format>
#include <utility>

namespace pigpen::ui {
namespace {

[[nodiscard]] cli::OptionParser option_parser(GuiOptions &options) {
  cli::OptionParser parser;
  parser.text("--model", "NAME", options.config.model,
              "Populate the model field and auto-start",
              cli::EmptyText::rejected);
  parser.text("--base-url", "URL", options.config.base_url,
              std::format("Populate the model endpoint field (default: {})",
                          options.config.base_url),
              cli::EmptyText::rejected);
  cli::add_reward_option(parser, options.reward_weights);
  parser.flag("--help", options.help, true, "Show this help and exit");
  return parser;
}

} // namespace

std::expected<GuiOptions, std::string>
parse_gui_options(const std::span<const std::string_view> arguments) {
  GuiOptions options;
  auto parser = option_parser(options);
  if (auto parsed = parser.parse(arguments); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return options;
}

std::string gui_options_help() {
  GuiOptions defaults;
  return option_parser(defaults).help();
}

} // namespace pigpen::ui
