/// @file job_spec.hpp
/// @brief One worker job as a trainer writes it on the worker's stdin: a
/// JSON object parsed into plain fields.
///
/// Parsing is here because nlohmann/json is private to pigpen_agent; the
/// defaults, rollout-id rules, and uniqueness are the caller's (the worker's
/// job stream), which sees only this struct.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief A job line's fields, exactly as given; absent keys stay absent.
struct JobSpec {
  std::uint64_t seed{};
  std::optional<std::uint32_t> sample{};
  std::optional<std::string> rollout_id{};
  /// Absent both when the key is missing and when it is null.
  std::optional<std::uint32_t> sampling_seed{};

  friend bool operator==(const JobSpec &, const JobSpec &) = default;
};

/// @brief Parse one job line: a JSON object with a required `seed`
/// (0..2^64-1) and optional `sample` (0..2^32-1), `rollout_id` (a string),
/// and `sampling_seed` (0..2^32-1 or null). Numbers must be written as
/// whole numbers, not with a fraction or exponent.
/// @return The fields, or a message: `invalid JSON at byte N`, `a job must
/// be a JSON object`, `unknown key "K"`, `"seed" is required`, or `"K" must
/// be ...` naming the expected type and range.
[[nodiscard]] std::expected<JobSpec, std::string>
parse_job_spec(std::string_view line);

} // namespace pigpen::agent
