/// @file tool_activity.hpp
/// @brief The typed record of one decoded world-tool invocation.
///
/// ToolActivity is Pig Pen's application-level account of what a tool call
/// did: which tool, what came of it, and the observable world transition.
/// It carries no JSON — provider encoding belongs to scry, and persistent
/// serialization belongs to MetricsWriter.
#pragma once

#include "world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief The closed set of world tools the model can invoke.
enum class ToolKind : std::uint8_t {
  move,
  look,
  eat,
};

/// @brief Number of ToolKind enumerators, for counter arrays.
inline constexpr std::size_t tool_kind_count{3};
static_assert(static_cast<std::size_t>(ToolKind::eat) + 1U == tool_kind_count,
              "tool_kind_count must track the ToolKind enumerators");

/// @brief What one decoded tool invocation amounted to.
/// @note Every value except budget_exhausted means the world executed the
/// action; budget_exhausted means Pig Pen's per-turn application budget
/// rejected the call before it could touch the world.
enum class ToolOutcome : std::uint8_t {
  succeeded,
  blocked_by_wall,
  nothing_to_eat,
  budget_exhausted,
};

/// @brief Stable lowercase name used by the UI, the CLI, and the log.
[[nodiscard]] constexpr std::string_view tool_kind_name(ToolKind kind) noexcept;
/// @brief Stable lowercase name used by the UI, the CLI, and the log.
[[nodiscard]] constexpr std::string_view
tool_outcome_name(ToolOutcome outcome) noexcept;

/// @brief One successfully decoded world-tool invocation and its observable
/// world transition, as published by WorldToolController.
/// @note Application-budget rejections still appear here: they carry
/// ToolOutcome::budget_exhausted and identical before/after positions.
struct ToolActivity {
  /// Monotonic counter across the episode; stamped by the journal.
  std::uint64_t tick{};
  std::size_t turn{};
  ToolKind kind{ToolKind::move};
  ToolOutcome outcome{ToolOutcome::succeeded};
  world::Position before{};
  world::Position after{};
  std::optional<world::Direction> direction{};
  std::optional<world::ItemType> eaten{};
  /// Truthful cumulative score after the call, regardless of what the
  /// scenario flags let the model see.
  int score_after{};
  /// Human-readable one-liner built once at publication; always truthful.
  std::string summary{};

  /// @brief Whether the world executed the action (even unsuccessfully),
  /// as opposed to the budget rejecting it outright.
  [[nodiscard]] constexpr bool action_executed() const noexcept {
    return outcome != ToolOutcome::budget_exhausted;
  }
  /// @brief Whether the action ran and achieved its purpose.
  [[nodiscard]] constexpr bool succeeded() const noexcept {
    return outcome == ToolOutcome::succeeded;
  }

  friend bool operator==(const ToolActivity &, const ToolActivity &) = default;
};

/// @brief Consumer of published tool activities.
///
/// WorldToolController publishes through this seam so it can be tested with
/// a recording sink, while Session wires it to the ToolActivityJournal.
class IToolActivitySink {
public:
  virtual ~IToolActivitySink() = default;

  /// @brief Accept one activity. Called synchronously from the thread
  /// driving Session::pump(); implementations must not re-enter the tool
  /// dispatch and must not throw.
  virtual void publish(ToolActivity activity) = 0;

protected:
  IToolActivitySink() = default;
  IToolActivitySink(const IToolActivitySink &) = default;
  IToolActivitySink &operator=(const IToolActivitySink &) = default;
};

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
  case ToolOutcome::budget_exhausted:
    return "budget_exhausted";
  }
  return "unknown";
}

} // namespace pigpen::agent
