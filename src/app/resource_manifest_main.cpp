/// @file resource_manifest_main.cpp
/// @brief Writes compiled prompts and reflected tool contracts as a build
/// artifact.
#include "agent/prompt.hpp"
#include "agent/prompt_text.hpp"
#include "agent/tool_definitions.hpp"
#include "text/catalog.hpp"

#include <nlohmann/json.hpp>
#include <scry/reflection.hpp>

#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
using nlohmann::json;

/// Serialize the already-decoded catalog, without reopening its source file.
template <typename Catalog> json catalog_json(const Catalog &catalog) {
  auto result = json::object();
  for (const auto &entry : catalog.entries) {
    const std::string key{catalog.data.data() + entry.key_offset,
                          entry.key_size};
    result[key] = catalog.get(key);
  }
  return result;
}

template <typename Value> json reflected_json(const Value &value) {
  const auto encoded = scry::reflection::encode(value);
  if (!encoded) {
    throw std::runtime_error{encoded.error().message};
  }
  return json::parse(encoded->text);
}

json manifest() {
  namespace agent = pigpen::agent;
  auto tools = json::array();
  agent::for_each_tool_definition([&]<typename Arguments>(
                                      agent::ToolKind kind,
                                      std::string_view description,
                                      auto invoke) {
    using Execution =
        std::invoke_result_t<decltype(invoke), agent::WorldTools &, Arguments>;
    using Response = typename Execution::response_type;
    tools.push_back({
        {"name", agent::tool_kind_name(kind)},
        {"description", description},
        {"input_schema",
         json::parse(scry::reflection::input_schema_v<Arguments>)},
        {"result_schema",
         json::parse(scry::reflection::input_schema_v<Response>)},
    });
  });

  const agent::Config defaults;
  auto systems = json::array();
  for (const bool known : {false, true}) {
    for (const bool opaque : {false, true}) {
      for (const bool feedback : {false, true}) {
        auto config = defaults;
        config.known_item_values = known;
        config.opaque_look = opaque;
        config.reward_feedback = feedback;
        systems.push_back({{"config", reflected_json(config)},
                           {"text", agent::build_system_prompt(config)}});
      }
    }
  }
  static constexpr unsigned char cli_source[] = {
#embed "../../resources/cli.json"
  };
  static constexpr auto cli = pigpen::text::make_catalog<cli_source>();
  return {
      {"format_version", 1},
      {"build",
       {{"project_version", PIGPEN_VERSION},
        {"configuration", PIGPEN_BUILD_CONFIGURATION},
        {"compiler", __VERSION__},
        {"cplusplus", __cplusplus}}},
      {"default_config", reflected_json(defaults)},
      {"max_world_tool_calls_per_turn", agent::max_world_tool_calls_per_turn},
      {"prompt_templates", catalog_json(agent::prompt_text::detail::catalog)},
      {"cli_text", catalog_json(cli)},
      {"tools", std::move(tools)},
      {"system_prompt_examples", std::move(systems)},
      {"turn_prompt_examples",
       {{"turn", 1},
        {"turn_budget", defaults.turn_budget},
        {"automatic", agent::build_turn_prompt(1, defaults.turn_budget)},
        {"zero_tool_recovery",
         agent::build_turn_prompt(1, defaults.turn_budget, {}, true)}}},
  };
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: pigpen-resource-manifest OUTPUT.json\n";
    return 2;
  }
  try {
    const auto contents = manifest().dump(2) + '\n';
    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(argv[1], std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
  } catch (const std::exception &error) {
    std::cerr << "Cannot write build resource manifest: " << error.what()
              << '\n';
    return 1;
  }
}
