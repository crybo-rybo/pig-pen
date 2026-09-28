/// @file config_options.cpp
/// @brief Shared CLI flag registration; the contract is in the header.
#include "cli/config_options.hpp"

#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>

namespace pigpen::cli {

std::string api_key_from_environment() {
  const auto *const value = std::getenv(std::string{api_key_variable}.c_str());
  return value == nullptr ? std::string{} : std::string{value};
}

void add_reward_option(OptionParser &parser, agent::RewardWeights &weights) {
  std::string description{
      "Override one reward weight; repeatable. NAME is one of"};
  for (std::size_t index = 0; index < agent::reward_weight_fields.size();
       ++index) {
    description += index == 0 ? " " : ", ";
    description += agent::reward_weight_fields[index].name;
  }
  parser.value("--reward", "NAME=VALUE", std::move(description),
               [&weights](const std::string_view assignment) {
                 return agent::parse_reward_weight(assignment, weights);
               });
}

void add_config_options(OptionParser &parser, agent::Config &config,
                        agent::SessionOptions &options) {
  constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
  parser.text("--base-url", "URL", config.base_url,
              std::format("Model endpoint (default: {})", config.base_url));
  parser.text("--model", "NAME", config.model,
              "Exact model identifier sent to the server (required)");
  parser.integer(
      "--seed", "INTEGER", config.seed, 0,
      std::numeric_limits<std::uint64_t>::max(),
      std::format("Deterministic world seed (default: {})", config.seed));
  parser.integer("--turns", "INTEGER", config.turn_budget, 1,
                 agent::turn_budget_limit,
                 std::format("Episode turn budget, 1..{} (default: {})",
                             agent::turn_budget_limit, config.turn_budget));
  parser.integer("--max-tool-rounds", "INTEGER", config.max_tool_rounds, 1,
                 agent::tool_rounds_limit,
                 std::format("Tool rounds per turn, 1..{} (default: {})",
                             agent::tool_rounds_limit, config.max_tool_rounds));
  parser.real("--temperature", "NUMBER", config.temperature, 0.0, 2.0,
              std::format("Sampling temperature, {}..{} (default: {})",
                          format_real(0.0), format_real(2.0),
                          format_real(config.temperature)));
  parser.integer(
      "--sampling-seed", "INTEGER", config.sampling_seed, 0, u32_max,
      std::format("Provider sampling seed, 0..{} (default: {})", u32_max,
                  config.sampling_seed ? std::to_string(*config.sampling_seed)
                                       : std::string{"unset"}));
  parser.flag("--hidden-values", config.known_item_values, false,
              "Omit item values from the system prompt");
  parser.flag("--no-reward-feedback", config.reward_feedback, false,
              "Hide numeric reward and score from eat results");
  parser.flag("--opaque-look", config.opaque_look, true,
              "Report occupied cells as 'something'");
  parser.text("--prompt-variant", "NAME", options.prompt_variant,
              std::format("Label recorded in the metrics header (default: {})",
                          options.prompt_variant));
  add_reward_option(parser, options.reward_weights);
}

ParseResult validate_config_options(const agent::Config &config,
                                    const agent::SessionOptions &options) {
  if (config.base_url.empty()) {
    return std::unexpected("--base-url cannot be empty");
  }
  if (config.model.empty()) {
    return std::unexpected("--model is required");
  }
  if (options.log_directory && options.log_directory->empty()) {
    return std::unexpected("--log-dir cannot be empty");
  }
  if (options.prompt_variant.empty()) {
    return std::unexpected("--prompt-variant cannot be empty");
  }
  return {};
}

} // namespace pigpen::cli
