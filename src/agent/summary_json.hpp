/// @file summary_json.hpp
/// @brief The one serialisation of EpisodeSummary, its reward, and its
/// weights; the log footer and any front end share it.
#pragma once

#include "agent/episode_summary.hpp"

#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief @p summary as one compact JSON object with sorted keys and no
/// trailing newline.
///
/// Keys: `type` (@p record_type), `complete`, `finish_reason` (null while
/// unfinished), `error`, `final_score`, `items_eaten`, `tool_call_counts`,
/// `turns_used`, `duration_ms`, `calls`, `reward`, `reward_version`, and
/// `reward_weights`. `reward.total` is null when the reward is invalid, and
/// `reward.invalid_reason` is null when it is valid; the raw counts and
/// `terms` are always present.
[[nodiscard]] std::string
to_json_line(const EpisodeSummary &summary,
             std::string_view record_type = "summary");

} // namespace pigpen::agent
