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

namespace pigpen::core {

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
[[nodiscard]] constexpr std::string_view
tool_kind_name(const ToolKind kind) noexcept {
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

/// @brief Stable presentation/log spelling of @p outcome.
[[nodiscard]] constexpr std::string_view
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

/// @brief One successfully decoded world-tool handler invocation and its
/// observable world transition.
/// @note Calls that the tool loop or the binding refuses never enter this
/// feed. JSON strings are the exact canonical arguments and the exact result
/// text that went back to the model; use typed fields for application
/// behavior.
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

  /// The call identity from the tool loop: the backend's call id, the round
  /// (starts at one), and the index in the batch (starts at zero).
  std::string call_id{};
  std::uint32_t round{};
  std::uint32_t index{};

  /// @brief Whether the action ran and achieved its purpose.
  [[nodiscard]] constexpr bool succeeded() const noexcept {
    return outcome == ToolOutcome::succeeded;
  }
};

/// @brief Append-only activity history; indices are stable for the lifetime
/// of a session.
using ToolActivityFeed = std::vector<ToolActivity>;

} // namespace pigpen::core
