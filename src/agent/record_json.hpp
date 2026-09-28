/// @file record_json.hpp
/// @brief JSON fragments that more than one record shares, as nlohmann
/// values.
///
/// Internal to pigpen_agent: nlohmann/json is a private dependency, so only
/// agent translation units include this header. Front ends and tests use the
/// string API in summary_json.hpp.
#pragma once

#include "agent/config.hpp"
#include "world/world.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace pigpen::agent {

/// @brief @p record as one compact line. Invalid UTF-8 anywhere in it is
/// written as U+FFFD rather than thrown, so a bad byte from any source
/// cannot abort a log or a worker record.
[[nodiscard]] std::string dump_line(const nlohmann::json &record);

/// @brief @p position in the logs' `{"x", "y"}` shape.
[[nodiscard]] nlohmann::json position_json(world::Position position);

/// @brief @p config as the log header and the worker's `episode` record
/// both write it: `base_url`, `model`, `seed`, `temperature`,
/// `max_output_tokens`, `sampling_seed` (null when unset), and `scenario`
/// (grid, spawn, item counts, budgets, and the three visibility flags).
[[nodiscard]] nlohmann::json config_json(const Config &config);

} // namespace pigpen::agent
