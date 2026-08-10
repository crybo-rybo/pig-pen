/// @file tool_activity_journal.cpp
/// @brief ToolActivityJournal implementation; the contract is in the header.
#include "agent/tool_activity_journal.hpp"

#include <exception>
#include <numeric>
#include <utility>

namespace pigpen::agent {
namespace {

[[nodiscard]] constexpr std::size_t kind_index(const ToolKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

[[nodiscard]] constexpr std::size_t
item_index(const world::ItemType item) noexcept {
  return static_cast<std::size_t>(item);
}

} // namespace

ToolActivityJournal::ToolActivityJournal(Persist persist)
    : persist_(std::move(persist)) {}

void ToolActivityJournal::publish(ToolActivity activity) {
  activity.tick = next_tick_++;
  ++call_counts_[kind_index(activity.kind)];
  if (activity.eaten) {
    ++eaten_counts_[item_index(*activity.eaten)];
  }
  activities_.push_back(std::move(activity));

  // Persist after committing so a failing writer can never suppress the
  // activity itself; the first failure is latched and later consumed by
  // Session::pump() to fail the episode outside tool dispatch. A throwing
  // hook is absorbed for the same reason: an exception escaping here would
  // make scry discard the committed typed response as a handler error.
  if (persist_ && persistence_error_.empty()) {
    try {
      if (auto persisted = persist_(activities_.back()); !persisted) {
        persistence_error_ = std::move(persisted.error());
        persistence_failure_pending_ = true;
      }
    } catch (const std::exception &error) {
      persistence_error_ = error.what();
      persistence_failure_pending_ = true;
    } catch (...) {
      persistence_error_ = "activity persistence threw an unknown exception";
      persistence_failure_pending_ = true;
    }
  }
}

const std::vector<ToolActivity> &
ToolActivityJournal::activities() const noexcept {
  return activities_;
}

std::size_t ToolActivityJournal::call_count() const noexcept {
  return activities_.size();
}

std::size_t
ToolActivityJournal::call_count(const ToolKind kind) const noexcept {
  return call_counts_[kind_index(kind)];
}

std::size_t
ToolActivityJournal::eaten_count(const world::ItemType item) const noexcept {
  return eaten_counts_[item_index(item)];
}

std::size_t ToolActivityJournal::eaten_total() const noexcept {
  return std::accumulate(eaten_counts_.begin(), eaten_counts_.end(),
                         std::size_t{0});
}

const std::string &ToolActivityJournal::persistence_error() const noexcept {
  return persistence_error_;
}

bool ToolActivityJournal::take_persistence_failure() noexcept {
  return std::exchange(persistence_failure_pending_, false);
}

} // namespace pigpen::agent
