/// @file config.hpp
/// @brief Episode settings shared verbatim by the GUI and headless front
/// ends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pigpen::agent {

/// @brief Hard limit on tool requests per turn, enforced by Scry before
/// dispatch; unknown tools and invalid arguments also spend this budget.
inline constexpr std::size_t max_world_tool_calls_per_turn{4};

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

} // namespace pigpen::agent
