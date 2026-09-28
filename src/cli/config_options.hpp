/// @file config_options.hpp
/// @brief The command-line flags every CLI shares: the whole episode Config
/// plus the SessionOptions a user sets directly.
#pragma once

#include "agent/config.hpp"
#include "agent/reward.hpp"
#include "agent/session_options.hpp"
#include "cli/option_parser.hpp"

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

/// @brief Register every Config flag (`--base-url`, `--model`, `--seed`,
/// `--turns`, `--max-tool-rounds`, `--temperature`, `--sampling-seed`,
/// `--hidden-values`, `--no-reward-feedback`, `--opaque-look`) plus
/// `--prompt-variant` and `--reward`.
/// @note Help text states the current values of @p config and @p options
/// as the defaults, so register before parsing.
void add_config_options(OptionParser &parser, agent::Config &config,
                        agent::SessionOptions &options);

/// @brief Reject what a parse can leave empty, in this order: `--base-url`,
/// `--model` (required), `--log-dir` (only when a log directory is set),
/// and `--prompt-variant`.
[[nodiscard]] ParseResult
validate_config_options(const agent::Config &config,
                        const agent::SessionOptions &options);

} // namespace pigpen::cli
