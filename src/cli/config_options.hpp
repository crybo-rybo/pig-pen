/// @file config_options.hpp
/// @brief The command-line flags every CLI shares: the whole episode Config
/// plus the SessionOptions a user sets directly.
#pragma once

#include "agent/config.hpp"
#include "agent/reward.hpp"
#include "agent/session_options.hpp"
#include "cli/option_parser.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace pigpen::cli {

/// @brief Environment variable that supplies SessionOptions::api_key.
inline constexpr std::string_view api_key_variable{"PIGPEN_API_KEY"};

/// @brief The api_key_variable value, or empty when unset.
[[nodiscard]] std::string api_key_from_environment();

/// @brief Register the repeatable `--reward NAME=VALUE` override. The help
/// text lists every name in agent::reward_weight_fields.
void add_reward_option(OptionParser &parser, agent::RewardWeights &weights);

/// @brief Whether add_config_options() registers `--seed`.
enum class WorldSeedOption : std::uint8_t {
  /// `--seed INTEGER` sets Config::seed.
  registered,
  /// The front end chooses world seeds itself (the worker's `--seeds`).
  omitted,
};

/// @brief Register every Config flag (`--base-url`, `--model`, `--seed`
/// unless omitted, `--turns`, `--max-tool-rounds`, `--temperature`,
/// `--sampling-seed`, `--hidden-values`, `--no-reward-feedback`,
/// `--opaque-look`) plus `--prompt-variant` and `--reward`.
/// @note Help text states the current values of @p config and @p options
/// as the defaults, so register before parsing.
void add_config_options(
    OptionParser &parser, agent::Config &config, agent::SessionOptions &options,
    WorldSeedOption world_seed = WorldSeedOption::registered);

/// @brief Reject what a parse can leave empty or malformed, in this order:
/// `--base-url` (empty, then not UTF-8), `--model` (required, then not
/// UTF-8), `--log-dir` (only when a log directory is set, and empty), and
/// `--prompt-variant` (empty, then not UTF-8). The UTF-8 checks keep every
/// string the log and records write serialisable.
[[nodiscard]] ParseResult
validate_config_options(const agent::Config &config,
                        const agent::SessionOptions &options);

} // namespace pigpen::cli
