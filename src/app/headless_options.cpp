/// @file headless_options.cpp
/// @brief Implements command-line parsing for the headless entry point.
#include "app/headless_options.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace pigpen::app {
namespace {

constexpr std::uint32_t max_timeout_seconds{86'400};

[[nodiscard]] std::expected<double, std::string>
parse_temperature(const std::string_view value) {
  double parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed,
                      std::chars_format::general);
  if (value.empty() || error != std::errc{} ||
      end != value.data() + value.size() || !std::isfinite(parsed) ||
      parsed < agent::min_temperature || parsed > agent::max_temperature) {
    return std::unexpected(
        "--temperature must be a finite number in the range 0.0..2.0");
  }
  return parsed;
}

template <typename Integer>
[[nodiscard]] std::expected<Integer, std::string>
parse_unsigned(const std::string_view value, const std::string_view option,
               const std::uint64_t minimum, const std::uint64_t maximum) {
  static_assert(std::is_integral_v<Integer> && std::is_unsigned_v<Integer>);
  if (value.empty() || value.front() == '-' || value.front() == '+') {
    return std::unexpected(std::string{option} +
                           " requires an unsigned decimal integer");
  }

  std::uint64_t parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed < minimum || parsed > maximum ||
      parsed >
          static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
    return std::unexpected(std::string{option} + " must be in the range " +
                           std::to_string(minimum) + ".." +
                           std::to_string(maximum));
  }
  return static_cast<Integer>(parsed);
}

[[nodiscard]] std::string
headless_config_error(const agent::ConfigValidationError error) {
  switch (error) {
  case agent::ConfigValidationError::empty_base_url:
    return "--base-url cannot be empty";
  case agent::ConfigValidationError::empty_model:
    return "--model is required";
  case agent::ConfigValidationError::turn_budget_too_small:
  case agent::ConfigValidationError::turn_budget_too_large:
    return "--turns must be in the range 1..10000";
  case agent::ConfigValidationError::tool_rounds_too_small:
  case agent::ConfigValidationError::tool_rounds_too_large:
    return "--max-tool-rounds must be in the range 1..64";
  case agent::ConfigValidationError::output_tokens_too_small:
    return std::string{agent::config_validation_message(error)};
  case agent::ConfigValidationError::temperature_out_of_range:
    return "--temperature must be a finite number in the range 0.0..2.0";
  }
  return std::string{agent::config_validation_message(error)};
}

} // namespace

std::expected<HeadlessOptions, std::string>
parse_headless_options(const std::span<const std::string_view> arguments) {
  HeadlessOptions options;
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
      if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
        return std::unexpected(std::string{name} + " requires a value");
      }
      const auto next = arguments[index + 1];
      if (next.starts_with("--")) {
        return std::unexpected(std::string{name} + " requires a value");
      }
      ++index;
      return next;
    };
    const auto reject_inline_value = [&]() -> std::expected<void, std::string> {
      if (inline_value) {
        return std::unexpected(std::string{name} + " does not take a value");
      }
      return {};
    };

    if (name == "--help") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.help = true;
    } else if (name == "--hidden-values") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.config.known_item_values = false;
    } else if (name == "--no-reward-feedback") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.config.reward_feedback = false;
    } else if (name == "--opaque-look") {
      if (auto valid = reject_inline_value(); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      options.config.opaque_look = true;
    } else if (name == "--base-url") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.config.base_url = *parsed;
    } else if (name == "--model") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.config.model = *parsed;
    } else if (name == "--log-dir") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.log_directory = *parsed;
    } else if (name == "--prompt-variant") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.prompt_variant = *parsed;
    } else if (name == "--input") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      options.user_input = std::string{*parsed};
    } else if (name == "--seed") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::uint64_t>(
          *parsed, name, 0, std::numeric_limits<std::uint64_t>::max());
      if (!number) {
        return std::unexpected(std::move(number.error()));
      }
      options.config.seed = *number;
    } else if (name == "--turns") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::size_t>(
          *parsed, name, agent::min_turn_budget, agent::max_turn_budget);
      if (!number) {
        return std::unexpected(std::move(number.error()));
      }
      options.config.turn_budget = *number;
    } else if (name == "--max-tool-rounds") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number = parse_unsigned<std::uint32_t>(
          *parsed, name, agent::min_tool_rounds, agent::max_tool_rounds);
      if (!number) {
        return std::unexpected(std::move(number.error()));
      }
      options.config.max_tool_rounds = *number;
    } else if (name == "--temperature") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto temperature = parse_temperature(*parsed);
      if (!temperature) {
        return std::unexpected(std::move(temperature.error()));
      }
      options.config.temperature = *temperature;
    } else if (name == "--timeout-seconds") {
      auto parsed = value();
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      auto number =
          parse_unsigned<std::uint32_t>(*parsed, name, 1, max_timeout_seconds);
      if (!number) {
        return std::unexpected(std::move(number.error()));
      }
      options.timeout = std::chrono::seconds{*number};
    } else {
      return std::unexpected("unknown option: " + std::string{name});
    }
  }

  if (options.help) {
    return options;
  }
  if (const auto error = agent::validate_config(options.config)) {
    return std::unexpected(headless_config_error(*error));
  }
  if (options.log_directory.empty()) {
    return std::unexpected("--log-dir cannot be empty");
  }
  if (options.prompt_variant.empty()) {
    return std::unexpected("--prompt-variant cannot be empty");
  }
  return options;
}

} // namespace pigpen::app
