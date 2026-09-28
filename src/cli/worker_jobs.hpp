/// @file worker_jobs.hpp
/// @brief The worker's job list: world seeds × samples, each with its
/// rollout id and provider sampling seed, plus the request headers a user
/// may add. Pure functions over plain values, so they are unit-tested.
#pragma once

#include "cli/option_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pigpen::cli {

/// @brief Request header carrying a job's world seed, next to
/// agent::rollout_header_name.
inline constexpr std::string_view seed_header_name{"X-Pigpen-Seed"};

/// @brief Most jobs (seeds × samples) one worker invocation accepts; it also
/// bounds how far a `--seeds` range is expanded.
inline constexpr std::size_t max_worker_jobs{1'000'000};

/// @brief Most samples per world seed.
inline constexpr std::uint32_t max_worker_samples{10'000};

/// @brief One episode to play.
struct WorkerJob {
  std::uint64_t seed{};
  /// Zero-based sample index within the seed.
  std::uint32_t sample{};
  /// `<prefix>/<seed>/<sample>`.
  std::string rollout_id{};
  /// `--sampling-seed-base` plus the sample index, or absent without a base.
  std::optional<std::uint32_t> sampling_seed{};

  friend bool operator==(const WorkerJob &, const WorkerJob &) = default;
};

/// @brief Append the seeds of one `--seeds` value to @p seeds.
/// @param value Comma-separated entries, each a seed or an inclusive range
/// `A-B` with A <= B; every seed is in 0..2^64-1.
/// @return A message naming `--seeds` on an empty entry, a malformed or
/// descending entry, or more than max_worker_jobs seeds in total; @p seeds
/// is then unchanged.
[[nodiscard]] ParseResult parse_seed_list(std::string_view value,
                                          std::vector<std::uint64_t> &seeds);

/// @brief Whether @p prefix is a usable rollout prefix: non-empty visible
/// ASCII (0x21..0x7E), so every rollout id is a valid header value.
[[nodiscard]] ParseResult validate_rollout_prefix(std::string_view prefix);

/// @brief The jobs for seeds × samples, seed-major in the order the seeds
/// were given. Each job is built on demand from its index, so a large batch
/// holds only its seed list, never a million rollout ids.
class WorkerJobs final {
public:
  /// @return The job list, or a message when @p seeds is empty, lists a
  /// seed twice (rollout ids must be unique; the smallest repeated seed is
  /// named), gives more than max_worker_jobs jobs, or when
  /// @p sampling_seed_base plus the last sample index exceeds 2^32-1.
  [[nodiscard]] static std::expected<WorkerJobs, std::string>
  create(std::vector<std::uint64_t> seeds, std::uint32_t samples,
         std::string prefix, std::optional<std::uint32_t> sampling_seed_base);

  /// @brief seeds × samples.
  [[nodiscard]] std::size_t size() const noexcept;
  /// @brief Job @p index: seed `index / samples`, sample `index % samples`.
  /// @pre index < size().
  [[nodiscard]] WorkerJob at(std::size_t index) const;

private:
  WorkerJobs(std::vector<std::uint64_t> seeds, std::uint32_t samples,
             std::string prefix,
             std::optional<std::uint32_t> sampling_seed_base);

  std::vector<std::uint64_t> seeds_;
  std::uint32_t samples_;
  std::string prefix_;
  std::optional<std::uint32_t> sampling_seed_base_;
};

/// @brief Parse one `--header NAME=VALUE` and append it to @p headers.
/// @return A message when `=` or the name is missing, or when the name is
/// one Pig Pen sets itself (`X-Pigpen-Rollout`, `X-Pigpen-Seed`, in any
/// letter case). Anything else is left to the model client, which rejects a
/// malformed header or one it manages when the first session is created.
[[nodiscard]] ParseResult
parse_request_header(std::string_view assignment,
                     std::vector<std::pair<std::string, std::string>> &headers);

} // namespace pigpen::cli
