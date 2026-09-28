/// @file summary_json.hpp
/// @brief The one serialisation of EpisodeSummary, its reward, and its
/// weights, and of the records built on it; the log footer and every front
/// end share it.
#pragma once

#include "agent/config.hpp"
#include "agent/episode_summary.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief @p summary as one compact JSON object with sorted keys and no
/// trailing newline.
///
/// Keys: `type` (@p record_type), `rollout_id` (null when empty),
/// `complete`, `finish_reason` (null while unfinished), `error`,
/// `final_score`, `items_eaten`, `tool_call_counts`, `turns_used`,
/// `duration_ms`, `calls`, `reward`, `reward_version`, and
/// `reward_weights`. `reward.total` is null when the reward is invalid, and
/// `reward.invalid_reason` is null when it is valid; the raw counts and
/// `terms` are always present.
[[nodiscard]] std::string
to_json_line(const EpisodeSummary &summary,
             std::string_view record_type = "summary");

/// @brief One worker job's outcome: its summary plus what identifies it.
struct EpisodeRecord {
  /// Its rollout_id is the job's rollout id.
  EpisodeSummary summary{};
  /// The episode's full Config, including its world and sampling seeds.
  Config config{};
  /// Zero-based sample index within the job's world seed.
  std::uint32_t sample{};
};

/// @brief @p record as the worker's `episode` line: every key of the
/// summary's line with `type` `episode`, plus `seed` and `sampling_seed`
/// (null when unset) copied from the config, `sample`, and `config` in the
/// log header's shape.
[[nodiscard]] std::string to_json_line(const EpisodeRecord &record);

/// @brief What the worker reports once its batch has ended.
struct BatchRecord {
  /// `completed`, `interrupted` (a signal), or `aborted` (a session could
  /// not be created).
  std::string status{};
  std::size_t jobs{};
  /// `episode` records written; every started job writes exactly one.
  std::size_t episodes{};
  std::size_t valid{};
  std::size_t invalid{};
  /// Jobs that never got an episode: queued when the batch stopped, or the
  /// one whose session could not be created.
  std::size_t not_started{};
  /// Job lines read from stdin that were rejected, each with its own
  /// `job_error` record; always 0 for jobs from the command line.
  std::size_t job_errors{};
  std::chrono::milliseconds duration{};
  /// Why the batch was aborted, or empty.
  std::string error{};
  /// The process exit status the worker returns.
  int exit_code{};
};

/// @brief @p record as the worker's final `batch` line; an empty `error` is
/// written as null.
[[nodiscard]] std::string to_json_line(const BatchRecord &record);

/// @brief A job line the worker rejected.
struct JobErrorRecord {
  /// One-based line number on stdin, counting every line read.
  std::size_t line{};
  /// Why the line is not a job.
  std::string error{};
};

/// @brief @p record as the worker's `job_error` line: `type`, `line`, and
/// `error`.
[[nodiscard]] std::string to_json_line(const JobErrorRecord &record);

} // namespace pigpen::agent
