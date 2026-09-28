/// @file gui_options.cpp
/// @brief Implements command-line parsing for the GUI entry point.
#include "ui/gui_options.hpp"

#include <optional>
#include <utility>

namespace pigpen::ui {

std::expected<GuiOptions, std::string>
parse_gui_options(const std::span<const std::string_view> arguments) {
  using Result = std::expected<void, std::string>;
  GuiOptions options;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (!argument.starts_with("--")) {
      return std::unexpected("unexpected positional argument: " +
                             std::string{argument});
    }
    const auto equals = argument.find('=');
    const std::string name{argument.substr(0, equals)};
    const auto inline_value = equals == std::string_view::npos
                                  ? std::optional<std::string_view>{}
                                  : argument.substr(equals + 1);

    const auto text = [&](std::string &target) -> Result {
      if (inline_value && !inline_value->empty()) {
        target = *inline_value;
      } else if (!inline_value && index + 1 < arguments.size() &&
                 !arguments[index + 1].empty() &&
                 !arguments[index + 1].starts_with("--")) {
        target = arguments[++index];
      } else {
        return std::unexpected(name + " requires a value");
      }
      return {};
    };

    Result result;
    if (name == "--help" && !inline_value) {
      options.help = true;
    } else if (name == "--help") {
      result = std::unexpected("--help does not take a value");
    } else if (name == "--model") {
      result = text(options.config.model);
    } else if (name == "--base-url") {
      result = text(options.config.base_url);
    } else {
      result = std::unexpected("unknown option: " + name);
    }
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
  }
  return options;
}

} // namespace pigpen::ui
