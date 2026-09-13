/// @file activity_history.hpp
/// @brief Incremental, ImGui-free indexes over the append-only tool feed.
#pragma once

#include "agent/events.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pigpen::ui {

/// @brief A half-open range of activity-feed indices for one model turn.
struct ActivityRange final {
  std::size_t begin{};
  std::size_t end{};

  friend bool operator==(const ActivityRange &,
                         const ActivityRange &) = default;
};

/// @brief Counts derived incrementally from decoded tool activities.
struct ActivityCounts final {
  std::size_t moves{};
  std::size_t looks{};
  std::size_t eat_attempts{};
  std::size_t successful_eats{};
  std::size_t failed_eats{};

  friend bool operator==(const ActivityCounts &,
                         const ActivityCounts &) = default;
};

/// @brief Display strings cached once when an activity is appended.
struct ActivityPresentation final {
  std::string short_arguments{};
  std::string short_result{};
  std::string transcript_arguments{};
  std::string transcript_result{};
};

/// @brief Incremental UI indexes for an append-only ToolActivityFeed.
///
/// synchronize() only processes newly appended records. reset() drops all
/// feed-derived state while retaining the active filter, which makes replacing
/// a Session cheap and keeps the filter field's visible behavior consistent.
class ActivityHistory final {
public:
  void synchronize(const agent::ToolActivityFeed &activities) {
    if (activities.size() < indexed_size_) {
      reset();
    }

    for (auto index = indexed_size_; index < activities.size(); ++index) {
      const auto &activity = activities[index];
      auto [range, inserted] = ranges_by_turn_.try_emplace(
          activity.turn, ActivityRange{.begin = index, .end = index + 1U});
      if (!inserted) {
        // Tool calls arrive in turn order, so each turn occupies one
        // contiguous range in the append-only feed.
        range->second.end = index + 1U;
      }

      switch (activity.kind) {
      case agent::ToolKind::move:
        ++counts_.moves;
        break;
      case agent::ToolKind::look:
        ++counts_.looks;
        break;
      case agent::ToolKind::eat:
        ++counts_.eat_attempts;
        if (activity.eaten) {
          ++counts_.successful_eats;
        } else {
          ++counts_.failed_eats;
        }
        break;
      }

      presentations_.push_back({
          .short_arguments = compact(activity.arguments_json, 150U),
          .short_result = compact(activity.result_json, 150U),
          .transcript_arguments = compact(activity.arguments_json, 72U),
          .transcript_result = compact(activity.result_json, 110U),
      });
      if (normalized_filter_.empty() ||
          searchable_text(activity).find(normalized_filter_) !=
              std::string::npos) {
        filtered_indices_.push_back(index);
      }
    }
    indexed_size_ = activities.size();
  }

  /// @brief Changes the filter and rebuilds matches only when its normalized
  /// value differs from the previous value.
  /// @return true when the normalized filter changed.
  bool set_filter(const std::string_view filter,
                  const agent::ToolActivityFeed &activities) {
    synchronize(activities);
    auto normalized = normalize(filter);
    if (normalized == normalized_filter_) {
      return false;
    }
    normalized_filter_ = std::move(normalized);
    filtered_indices_.clear();
    filtered_indices_.reserve(activities.size());
    for (std::size_t index = 0; index < activities.size(); ++index) {
      if (normalized_filter_.empty() ||
          searchable_text(activities[index]).find(normalized_filter_) !=
              std::string::npos) {
        filtered_indices_.push_back(index);
      }
    }
    return true;
  }

  /// @brief Clears indexes for a replacement feed but preserves the filter.
  void reset() {
    indexed_size_ = 0U;
    ranges_by_turn_.clear();
    presentations_.clear();
    filtered_indices_.clear();
    counts_ = {};
  }

  [[nodiscard]] std::optional<ActivityRange>
  range_for_turn(const std::size_t turn) const {
    const auto found = ranges_by_turn_.find(turn);
    if (found == ranges_by_turn_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  [[nodiscard]] const std::vector<std::size_t> &
  filtered_indices() const noexcept {
    return filtered_indices_;
  }

  [[nodiscard]] const ActivityPresentation &
  presentation(const std::size_t index) const {
    return presentations_.at(index);
  }

  [[nodiscard]] const ActivityCounts &counts() const noexcept {
    return counts_;
  }

  [[nodiscard]] std::size_t indexed_size() const noexcept {
    return indexed_size_;
  }

private:
  [[nodiscard]] static std::string normalize(const std::string_view value) {
    const auto not_space = [](const char character) {
      return std::isspace(static_cast<unsigned char>(character)) == 0;
    };
    const auto first = std::find_if(value.begin(), value.end(), not_space);
    const auto last =
        std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (first >= last) {
      return {};
    }

    std::string result{first, last};
    std::transform(result.begin(), result.end(), result.begin(),
                   [](const char character) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(character)));
                   });
    return result;
  }

  [[nodiscard]] static std::string
  searchable_text(const agent::ToolActivity &activity) {
    auto text = std::to_string(activity.tick) + " " +
                std::to_string(activity.turn) + " " +
                std::string{agent::tool_kind_name(activity.kind)} + " " +
                std::string{agent::tool_outcome_name(activity.outcome)} + " " +
                activity.arguments_json + " " + activity.result_json;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const char character) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(character)));
                   });
    return text;
  }

  [[nodiscard]] static std::string compact(const std::string_view value,
                                           const std::size_t maximum) {
    auto text = std::string{value};
    if (text.size() > maximum) {
      text.resize(maximum - 3U);
      text += "...";
    }
    return text;
  }

  std::size_t indexed_size_{};
  std::unordered_map<std::size_t, ActivityRange> ranges_by_turn_{};
  std::vector<ActivityPresentation> presentations_{};
  std::vector<std::size_t> filtered_indices_{};
  ActivityCounts counts_{};
  std::string normalized_filter_{};
};

} // namespace pigpen::ui
