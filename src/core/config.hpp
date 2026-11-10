/// @file config.hpp
/// @brief Episode settings shared verbatim by the GUI and headless front
/// ends.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pigpen::core {

/// @brief Hard limit on tool requests per turn, enforced by the tool loop
/// before dispatch; unknown tools and invalid arguments also spend this
/// budget.
inline constexpr std::uint32_t max_world_tool_calls_per_turn{4};
/// @brief Inclusive upper bounds Session::create accepts.
inline constexpr std::uint32_t turn_budget_limit{10'000};
inline constexpr std::uint32_t tool_rounds_limit{64};
/// @brief llamad carries max_tokens as a signed 32-bit integer.
inline constexpr std::uint32_t max_output_tokens_limit{2'147'483'647};

/// @brief The llamad default socket: `$XDG_RUNTIME_DIR/llamad.sock` when
/// that variable is set and not empty, else `/tmp/llamad-<uid>.sock`.
[[nodiscard]] std::string default_socket_path();

/// @note The three visibility flags change only what the model is told; the
/// world, the scoring, and the log always record the truth.
struct Config {
  /// Unix socket of the llamad daemon. llamad serves exactly one model, so
  /// the model identity comes from the daemon, not from the config.
  std::string socket_path{};
  std::uint64_t seed{};
  std::uint32_t turn_budget{20};
  std::uint32_t max_tool_rounds{8};
  std::uint32_t max_output_tokens{8'192};
  double temperature{0.0};
  /// Sampling seed, independent of the world seed. Sent as llamad's
  /// `SamplingParams.seed` when set and omitted when unset; repeatability is
  /// best-effort.
  std::optional<std::uint32_t> sampling_seed{};

  /// Include the item/reward table in the system prompt.
  bool known_item_values{true};
  /// Include numeric reward and cumulative score in successful eat results.
  bool reward_feedback{true};
  /// Report occupied cells as "something" rather than revealing item types.
  bool opaque_look{false};

  friend bool operator==(const Config &, const Config &) = default;
};

} // namespace pigpen::core
