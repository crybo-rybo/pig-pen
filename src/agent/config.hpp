#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pigpen::agent {

/// Runtime and experiment settings shared by the headless and GUI front ends.
struct Config {
  std::string base_url{"http://127.0.0.1:11434/v1"};
  std::string model{"qwen3:8b"};
  std::uint64_t seed{};
  std::size_t turn_budget{20};
  std::uint32_t max_tool_rounds{8};
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
