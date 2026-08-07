#include "ui/gui_options.hpp"

#include <optional>
#include <utility>

namespace pigpen::ui {

std::expected<GuiOptions, std::string>
parse_gui_options(const std::span<const std::string_view> arguments) {
  GuiOptions options;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (!argument.starts_with("--")) {
      return std::unexpected("unexpected positional argument: " +
                             std::string{argument});
    }

    const auto equals = argument.find('=');
    const auto name = argument.substr(0, equals);
    const auto inline_value = equals == std::string_view::npos
                                  ? std::optional<std::string_view>{}
                                  : std::optional{argument.substr(equals + 1)};
    const auto value = [&]() -> std::expected<std::string_view, std::string> {
      if (inline_value) {
        if (inline_value->empty()) {
          return std::unexpected(std::string{name} + " requires a value");
        }
        return *inline_value;
      }
      if (index + 1 >= arguments.size() || arguments[index + 1].empty() ||
          arguments[index + 1].starts_with("--")) {
        return std::unexpected(std::string{name} + " requires a value");
      }
      return arguments[++index];
    };

    if (name == "--help") {
      if (inline_value) {
        return std::unexpected("--help does not take a value");
      }
      options.help = true;
    } else if (name == "--model") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.config.model = *parsed;
    } else if (name == "--base-url") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.config.base_url = *parsed;
    } else {
      return std::unexpected("unknown option: " + std::string{name});
    }
  }

  if (options.config.model.size() >= 160U) {
    return std::unexpected("--model must be shorter than 160 characters");
  }
  if (options.config.base_url.size() >= 384U) {
    return std::unexpected("--base-url must be shorter than 384 characters");
  }
  return options;
}

} // namespace pigpen::ui
