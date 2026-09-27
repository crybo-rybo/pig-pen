/// @file events.hpp
/// @brief The append-only feed of decoded world-tool activity.
///
/// The UI, the animation, and the metrics writer all read the same feed, so
/// one successfully decoded handler invocation produces exactly one record
/// everywhere.
#pragma once

#include "world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::agent {

/// @brief The closed set of world tools exposed to the model.
enum class ToolKind : std::uint8_t {
  move,
  look,
  eat,
};

/// @brief The application-level outcome of one decoded invocation.
enum class ToolOutcome : std::uint8_t {
  succeeded,
  blocked_by_wall,
  nothing_to_eat,
};

/// @brief Stable provider/log spelling of @p kind.
[[nodiscard]] constexpr std::string_view tool_kind_name(ToolKind kind) noexcept;
/// @brief Stable presentation/log spelling of @p outcome.
[[nodiscard]] constexpr std::string_view
tool_outcome_name(ToolOutcome outcome) noexcept;

/// @brief One successfully decoded world-tool handler invocation and its
/// observable world transition.
/// @note Calls refused by Scry never enter this feed. JSON strings are exact
/// canonical dispatch payloads; use typed fields for application behavior.
struct ToolActivity {
  std::uint64_t tick{};
  std::size_t turn{};
  ToolKind kind{ToolKind::move};
  ToolOutcome outcome{ToolOutcome::succeeded};
  std::string arguments_json{"{}"};
  std::string result_json{"{}"};
  world::Position before{};
  world::Position after{};
  std::optional<world::Direction> direction{};
  std::optional<world::ItemType> eaten{};
  /// Truthful cumulative world score, even when reward feedback is hidden.
  int score_after{};

  /// Scry's call identity, copied from the contextual handler.
  std::uint64_t scry_turn_id{};
  std::string call_id{};
  std::uint32_t round{};
  std::uint32_t index{};
  /// False when a framework failure prevented a dispatch observation. The
  /// world transition still happened, but its JSON payloads are unavailable.
  bool result_dispatched{true};

  /// @brief Whether the action ran and achieved its purpose.
  [[nodiscard]] constexpr bool succeeded() const noexcept {
    return outcome == ToolOutcome::succeeded;
  }

  friend bool operator==(const ToolActivity &, const ToolActivity &) = default;
};

/// @brief Append-only activity history; indices are stable for the lifetime
/// of a session.
using ToolActivityFeed = std::vector<ToolActivity>;

constexpr std::string_view tool_kind_name(const ToolKind kind) noexcept {
  switch (kind) {
  case ToolKind::move:
    return "move";
  case ToolKind::look:
    return "look";
  case ToolKind::eat:
    return "eat";
  }
  return "unknown";
}

constexpr std::string_view
tool_outcome_name(const ToolOutcome outcome) noexcept {
  switch (outcome) {
  case ToolOutcome::succeeded:
    return "succeeded";
  case ToolOutcome::blocked_by_wall:
    return "blocked_by_wall";
  case ToolOutcome::nothing_to_eat:
    return "nothing_to_eat";
  }
  return "unknown";
}

} // namespace pigpen::agent
