/// @file job_spec.cpp
/// @brief Job line parsing; the contract is in the header.
#include "agent/job_spec.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <set>
#include <string>

namespace pigpen::agent {
namespace {

/// @brief @p value as a whole number in 0..@p maximum, if it is one.
[[nodiscard]] std::optional<std::uint64_t>
whole_number(const nlohmann::json &value, const std::uint64_t maximum) {
  if (!value.is_number_unsigned()) {
    return std::nullopt;
  }
  const auto number = value.get<std::uint64_t>();
  return number <= maximum ? std::optional{number} : std::nullopt;
}

[[nodiscard]] std::string range_error(const std::string_view key,
                                      const std::uint64_t maximum,
                                      const std::string_view prefix = "") {
  return '"' + std::string{key} + "\" must be " + std::string{prefix} +
         "an integer in 0.." + std::to_string(maximum);
}

} // namespace

std::expected<JobSpec, std::string>
parse_job_spec(const std::string_view line) {
  constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
  constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();
  if (const auto nul = line.find('\0'); nul != std::string_view::npos) {
    return std::unexpected("NUL byte at byte " + std::to_string(nul + 1));
  }
  nlohmann::json object;
  std::optional<std::string> repeated_key;
  try {
    std::set<std::string> keys;
    // Only the top-level object's keys matter: every known value is a
    // scalar, and anything else is rejected below.
    object = nlohmann::json::parse(
        line, [&](const int depth, const nlohmann::json::parse_event_t event,
                  const nlohmann::json &parsed) {
          if (event == nlohmann::json::parse_event_t::key && depth == 1 &&
              !keys.insert(parsed.get<std::string>()).second && !repeated_key) {
            repeated_key = parsed.get<std::string>();
          }
          return true;
        });
  } catch (const nlohmann::json::parse_error &error) {
    return std::unexpected("invalid JSON at byte " +
                           std::to_string(error.byte));
  } catch (const nlohmann::json::out_of_range &) {
    // A number too large for a double, such as 1e400.
    return std::unexpected("invalid JSON: a number is out of range");
  } catch (const nlohmann::json::exception &error) {
    return std::unexpected(std::string{"invalid JSON: "} + error.what());
  }
  if (repeated_key) {
    return std::unexpected("key \"" + *repeated_key + "\" appears twice");
  }
  if (!object.is_object()) {
    return std::unexpected("a job must be a JSON object");
  }
  for (const auto &[key, value] : object.items()) {
    if (key != "seed" && key != "sample" && key != "rollout_id" &&
        key != "sampling_seed") {
      return std::unexpected("unknown key \"" + key + '"');
    }
  }

  JobSpec spec;
  const auto seed = object.find("seed");
  if (seed == object.end()) {
    return std::unexpected("\"seed\" is required");
  }
  const auto seed_value = whole_number(*seed, u64_max);
  if (!seed_value) {
    return std::unexpected(range_error("seed", u64_max));
  }
  spec.seed = *seed_value;
  if (const auto sample = object.find("sample"); sample != object.end()) {
    const auto value = whole_number(*sample, u32_max);
    if (!value) {
      return std::unexpected(range_error("sample", u32_max));
    }
    spec.sample = static_cast<std::uint32_t>(*value);
  }
  if (const auto id = object.find("rollout_id"); id != object.end()) {
    if (!id->is_string()) {
      return std::unexpected("\"rollout_id\" must be a string");
    }
    spec.rollout_id = id->get<std::string>();
  }
  if (const auto sampling = object.find("sampling_seed");
      sampling != object.end() && !sampling->is_null()) {
    const auto value = whole_number(*sampling, u32_max);
    if (!value) {
      return std::unexpected(range_error("sampling_seed", u32_max, "null or "));
    }
    spec.sampling_seed = static_cast<std::uint32_t>(*value);
  }
  return spec;
}

} // namespace pigpen::agent
