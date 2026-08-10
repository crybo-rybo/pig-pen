/// @file tool_activity_journal.hpp
/// @brief The append-only journal of typed tool activities.
///
/// One journal is owned per Session. Every consumer — UI panels, animation,
/// headless printing, statistics, and metrics persistence — reconciles from
/// the same appended record, so a decoded handler invocation produces
/// exactly one activity everywhere.
#pragma once

#include "agent/tool_activity.hpp"
#include "world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// @brief Append-only activity history with derived counters.
///
/// The journal stamps each activity with a monotonic tick, maintains
/// per-tool call counts and per-item eaten counts, and forwards every
/// record to an optional persistence hook (Session wires it to
/// MetricsWriter::record_tool). Persistence failure never blocks the
/// activity from committing: the record is appended first, the failure is
/// latched, and Session::pump() fails the episode after the in-flight
/// dispatch has returned its typed response to scry.
class ToolActivityJournal final : public IToolActivitySink {
public:
  /// @brief Persistence hook invoked once per appended activity.
  /// @note Must not call back into publish(): the hook receives a reference
  /// into the journal's own storage, which a re-entrant append would
  /// invalidate mid-call.
  using Persist =
      std::function<std::expected<void, std::string>(const ToolActivity &)>;

  /// @brief An empty @p persist keeps the journal purely in-memory.
  explicit ToolActivityJournal(Persist persist = {});

  ToolActivityJournal(const ToolActivityJournal &) = delete;
  ToolActivityJournal &operator=(const ToolActivityJournal &) = delete;

  /// @brief Stamp the next tick, append, update counters, then persist.
  /// @note A throwing or failing persistence hook is absorbed into the
  /// latched persistence error; the activity itself always commits.
  void publish(ToolActivity activity) override;

  /// @brief Full activity history, oldest first; indices are stable for
  /// the lifetime of the session.
  [[nodiscard]] const std::vector<ToolActivity> &activities() const noexcept;
  /// @brief Total decoded world-tool invocations so far.
  [[nodiscard]] std::size_t call_count() const noexcept;
  /// @brief Decoded invocations of one tool.
  [[nodiscard]] std::size_t call_count(ToolKind kind) const noexcept;
  /// @brief Items of one kind eaten through the tools.
  [[nodiscard]] std::size_t eaten_count(world::ItemType item) const noexcept;
  /// @brief Total items eaten; equals the number of successful eat calls.
  [[nodiscard]] std::size_t eaten_total() const noexcept;

  /// @brief First persistence failure, or empty. A failure is terminal for
  /// the episode but never blocks activity publication.
  [[nodiscard]] const std::string &persistence_error() const noexcept;
  /// @brief True exactly once after a persistence failure is latched, so
  /// the session can fail the episode outside tool dispatch.
  [[nodiscard]] bool take_persistence_failure() noexcept;

private:
  std::vector<ToolActivity> activities_{};
  std::array<std::size_t, tool_kind_count> call_counts_{};
  std::array<std::size_t, world::item_type_count> eaten_counts_{};
  std::uint64_t next_tick_{1};
  Persist persist_{};
  std::string persistence_error_{};
  bool persistence_failure_pending_{false};
};

} // namespace pigpen::agent
