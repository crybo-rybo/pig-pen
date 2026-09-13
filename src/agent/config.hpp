/// @file config.hpp
/// @brief Episode settings shared verbatim by the GUI and headless front
/// ends.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief Hard application-side limit on world actions authorized during one
/// turn.
inline constexpr std::size_t max_world_tool_calls_per_turn{4};

/// @brief Supported runtime bounds, shared by validation and both front ends.
inline constexpr std::size_t min_turn_budget{1};
inline constexpr std::size_t max_turn_budget{10'000};
inline constexpr std::uint32_t min_tool_rounds{1};
inline constexpr std::uint32_t max_tool_rounds{64};
inline constexpr std::uint32_t min_output_tokens{1};
inline constexpr double min_temperature{0.0};
inline constexpr double max_temperature{2.0};

/// @brief Runtime and experiment settings shared by the headless and GUI
/// front ends.
/// @note The three visibility flags change only what the model is told; the
/// world, the scoring, and the log always record the truth.
struct Config {
  std::string base_url{"http://127.0.0.1:11434/v1"};
  /// Exact provider model identifier. It is forwarded without normalization.
  std::string model{};
  std::uint64_t seed{};
  std::size_t turn_budget{20};
  std::uint32_t max_tool_rounds{8};
  std::uint32_t max_output_tokens{8'096};
  double temperature{0.0};

  /// Include the item/reward table in the system prompt.
  bool known_item_values{true};
  /// Include numeric reward and cumulative score in successful eat results.
  bool reward_feedback{true};
  /// Report occupied cells as "something" rather than revealing item types.
  bool opaque_look{false};

  friend bool operator==(const Config &, const Config &) = default;
};

/// @brief A field-level reason that a Config cannot safely start a Session.
enum class ConfigValidationError : std::uint8_t {
  empty_base_url,
  empty_model,
  turn_budget_too_small,
  turn_budget_too_large,
  tool_rounds_too_small,
  tool_rounds_too_large,
  output_tokens_too_small,
  temperature_out_of_range,
};

/// @brief Validate runtime settings without creating resources or changing
/// state.
[[nodiscard]] constexpr std::optional<ConfigValidationError>
validate_config(const Config &config) noexcept {
  if (config.base_url.empty()) {
    return ConfigValidationError::empty_base_url;
  }
  if (config.model.empty()) {
    return ConfigValidationError::empty_model;
  }
  if (config.turn_budget < min_turn_budget) {
    return ConfigValidationError::turn_budget_too_small;
  }
  if (config.turn_budget > max_turn_budget) {
    return ConfigValidationError::turn_budget_too_large;
  }
  if (config.max_tool_rounds < min_tool_rounds) {
    return ConfigValidationError::tool_rounds_too_small;
  }
  if (config.max_tool_rounds > max_tool_rounds) {
    return ConfigValidationError::tool_rounds_too_large;
  }
  if (config.max_output_tokens < min_output_tokens) {
    return ConfigValidationError::output_tokens_too_small;
  }
  if (!std::isfinite(config.temperature) ||
      config.temperature < min_temperature ||
      config.temperature > max_temperature) {
    return ConfigValidationError::temperature_out_of_range;
  }
  return std::nullopt;
}

/// @brief Stable Session-facing diagnostic for a validation failure.
[[nodiscard]] constexpr std::string_view
config_validation_message(const ConfigValidationError error) noexcept {
  switch (error) {
  case ConfigValidationError::empty_base_url:
    return "base URL cannot be empty";
  case ConfigValidationError::empty_model:
    return "model cannot be empty";
  case ConfigValidationError::turn_budget_too_small:
    return "turn budget must be greater than zero";
  case ConfigValidationError::turn_budget_too_large:
    return "turn budget must not exceed 10000";
  case ConfigValidationError::tool_rounds_too_small:
    return "maximum tool rounds must be greater than zero";
  case ConfigValidationError::tool_rounds_too_large:
    return "maximum tool rounds must not exceed 64";
  case ConfigValidationError::output_tokens_too_small:
    return "maximum output tokens must be greater than zero";
  case ConfigValidationError::temperature_out_of_range:
    return "temperature must be finite and in the range 0..2";
  }
  return "invalid configuration";
}

} // namespace pigpen::agent
