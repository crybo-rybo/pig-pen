/// @file worker_jobs.cpp
/// @brief Worker job expansion; the contract is in the header.
#include "cli/worker_jobs.hpp"

#include "agent/job_spec.hpp"
#include "agent/session_options.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <limits>
#include <system_error>
#include <utility>

namespace pigpen::cli {
namespace {

/// @brief A whole decimal seed with no sign, whitespace, or suffix.
[[nodiscard]] std::optional<std::uint64_t> parse_seed(std::string_view text) {
  std::uint64_t seed{};
  const auto *const end = text.data() + text.size();
  if (const auto [last, error] = std::from_chars(text.data(), end, seed);
      error != std::errc{} || last != end || text.empty()) {
    return std::nullopt;
  }
  return seed;
}

/// @brief Non-empty visible ASCII (0x21..0x7E): a valid header value with
/// no spaces.
[[nodiscard]] bool is_visible_ascii(const std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x21U && byte <= 0x7EU;
  });
}

/// @brief ASCII case-insensitive equality, as HTTP header names compare.
[[nodiscard]] bool header_name_equal(const std::string_view left,
                                     const std::string_view right) {
  return std::ranges::equal(left, right, [](const char a, const char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

} // namespace

ParseResult parse_seed_list(const std::string_view value,
                            std::vector<std::uint64_t> &seeds) {
  std::vector<std::uint64_t> parsed;
  std::size_t position = 0;
  while (true) {
    const auto comma = value.find(',', position);
    const auto entry = value.substr(position, comma == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : comma - position);
    if (entry.empty()) {
      return std::unexpected("--seeds has an empty entry in \"" +
                             std::string{value} + '"');
    }
    const auto dash = entry.find('-');
    const auto first = parse_seed(entry.substr(0, dash));
    const auto last = dash == std::string_view::npos
                          ? first
                          : parse_seed(entry.substr(dash + 1));
    if (!first || !last) {
      return std::unexpected(
          "--seeds entry \"" + std::string{entry} +
          "\" is not a seed or an A-B range of seeds in 0.." +
          std::to_string(std::numeric_limits<std::uint64_t>::max()));
    }
    if (*first > *last) {
      return std::unexpected("--seeds range \"" + std::string{entry} +
                             "\" must not descend");
    }
    // Checked before expanding, so a huge range allocates nothing.
    const auto available =
        max_worker_jobs -
        std::min(max_worker_jobs, seeds.size() + parsed.size());
    if (*last - *first >= available) {
      return std::unexpected("--seeds lists more than " +
                             std::to_string(max_worker_jobs) + " seeds");
    }
    for (auto seed = *first;; ++seed) {
      parsed.push_back(seed);
      if (seed == *last) {
        break;
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    position = comma + 1;
  }
  seeds.insert(seeds.end(), parsed.begin(), parsed.end());
  return {};
}

ParseResult validate_rollout_prefix(const std::string_view prefix) {
  if (prefix.empty()) {
    return std::unexpected("--rollout-prefix cannot be empty");
  }
  if (!is_visible_ascii(prefix)) {
    return std::unexpected(
        "--rollout-prefix must be visible ASCII without spaces");
  }
  return {};
}

std::expected<WorkerJobs, std::string>
WorkerJobs::create(std::vector<std::uint64_t> seeds,
                   const std::uint32_t samples, std::string prefix,
                   const std::optional<std::uint32_t> sampling_seed_base) {
  if (seeds.empty()) {
    return std::unexpected("--seeds is required");
  }
  // Sorting a copy costs one more seed list, not a hash node per seed.
  auto sorted = seeds;
  std::ranges::sort(sorted);
  if (const auto repeated = std::ranges::adjacent_find(sorted);
      repeated != sorted.end()) {
    return std::unexpected("--seeds lists seed " + std::to_string(*repeated) +
                           " more than once");
  }
  if (samples == 0 || seeds.size() > max_worker_jobs / samples) {
    return std::unexpected("--seeds and --samples must give 1.." +
                           std::to_string(max_worker_jobs) + " jobs, not " +
                           std::to_string(seeds.size()) + " x " +
                           std::to_string(samples));
  }
  constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
  if (sampling_seed_base && *sampling_seed_base > u32_max - (samples - 1U)) {
    return std::unexpected("--sampling-seed-base " +
                           std::to_string(*sampling_seed_base) +
                           " leaves no room for " + std::to_string(samples) +
                           " samples: the last sampling seed must not exceed " +
                           std::to_string(u32_max));
  }
  return WorkerJobs{std::move(seeds), samples, std::move(prefix),
                    sampling_seed_base};
}

WorkerJobs::WorkerJobs(std::vector<std::uint64_t> seeds,
                       const std::uint32_t samples, std::string prefix,
                       const std::optional<std::uint32_t> sampling_seed_base)
    : seeds_(std::move(seeds)), samples_(samples), prefix_(std::move(prefix)),
      sampling_seed_base_(sampling_seed_base) {}

std::size_t WorkerJobs::size() const noexcept {
  return seeds_.size() * samples_;
}

WorkerJob WorkerJobs::at(const std::size_t index) const {
  const auto seed = seeds_[index / samples_];
  const auto sample = static_cast<std::uint32_t>(index % samples_);
  return {
      .seed = seed,
      .sample = sample,
      .rollout_id =
          prefix_ + '/' + std::to_string(seed) + '/' + std::to_string(sample),
      // create() checked that the last sample's seed fits.
      .sampling_seed =
          sampling_seed_base_
              ? std::optional<std::uint32_t>{*sampling_seed_base_ + sample}
              : std::nullopt,
  };
}

JobStream::JobStream(std::string prefix) : prefix_(std::move(prefix)) {}

std::expected<WorkerJob, std::string>
JobStream::accept(const std::string_view line, const std::size_t number) {
  try {
    return accept_or_throw(line, number);
  } catch (const std::exception &error) {
    // parse_job_spec() reports bad content itself; this is only a guard, so
    // no line can ever abort a long-lived worker.
    return std::unexpected(std::string{"could not read the job: "} +
                           error.what());
  }
}

std::expected<WorkerJob, std::string>
JobStream::accept_or_throw(const std::string_view line,
                           const std::size_t number) {
  auto spec = agent::parse_job_spec(line);
  if (!spec) {
    return std::unexpected(std::move(spec.error()));
  }
  const auto sample = spec->sample.value_or(0);
  auto rollout_id =
      spec->rollout_id.value_or(prefix_ + '/' + std::to_string(spec->seed) +
                                '/' + std::to_string(sample));
  if (!is_visible_ascii(rollout_id)) {
    return std::unexpected(
        "\"rollout_id\" must be non-empty visible ASCII without spaces");
  }
  if (const auto [used, inserted] =
          rollout_ids_.try_emplace(rollout_id, number);
      !inserted) {
    return std::unexpected("rollout id \"" + rollout_id +
                           "\" was already used on line " +
                           std::to_string(used->second));
  }
  return WorkerJob{
      .seed = spec->seed,
      .sample = sample,
      .rollout_id = std::move(rollout_id),
      .sampling_seed = spec->sampling_seed,
  };
}

ParseResult parse_request_header(
    const std::string_view assignment,
    std::vector<std::pair<std::string, std::string>> &headers) {
  const auto equals = assignment.find('=');
  if (equals == std::string_view::npos) {
    return std::unexpected("--header must be NAME=VALUE, not \"" +
                           std::string{assignment} + '"');
  }
  const auto name = assignment.substr(0, equals);
  if (name.empty()) {
    return std::unexpected("--header needs a name before '='");
  }
  for (const auto reserved : {agent::rollout_header_name, seed_header_name}) {
    if (header_name_equal(name, reserved)) {
      return std::unexpected("--header " + std::string{name} +
                             " is set by the worker itself");
    }
  }
  headers.emplace_back(std::string{name},
                       std::string{assignment.substr(equals + 1)});
  return {};
}

} // namespace pigpen::cli
