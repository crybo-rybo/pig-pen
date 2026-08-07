#include "agent/tool_executor.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace pigpen::agent {
namespace {

using JsonResult = std::expected<nlohmann::json, ToolError>;

[[nodiscard]] ToolError invalid_arguments(std::string message) {
  return {.code = ToolErrorCode::invalid_arguments,
          .message = std::move(message)};
}

[[nodiscard]] std::string_view error_code_name(const ToolErrorCode code) {
  switch (code) {
  case ToolErrorCode::unknown_tool:
    return "unknown_tool";
  case ToolErrorCode::malformed_json:
    return "malformed_json";
  case ToolErrorCode::invalid_arguments:
    return "invalid_arguments";
  }
  return "unknown";
}

[[nodiscard]] nlohmann::json error_result(const ToolError &error) {
  return {
      {"ok", false},
      {"error", error.message},
      {"error_code", error_code_name(error.code)},
  };
}

[[nodiscard]] std::optional<world::Direction>
parse_direction(const std::string_view direction) {
  if (direction == "north") {
    return world::Direction::north;
  }
  if (direction == "south") {
    return world::Direction::south;
  }
  if (direction == "east") {
    return world::Direction::east;
  }
  if (direction == "west") {
    return world::Direction::west;
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view
direction_name(const world::Direction direction) {
  switch (direction) {
  case world::Direction::north:
    return "north";
  case world::Direction::south:
    return "south";
  case world::Direction::east:
    return "east";
  case world::Direction::west:
    return "west";
  }
  return "";
}

[[nodiscard]] nlohmann::json position_json(const world::Position position) {
  return {{"x", position.x}, {"y", position.y}};
}

[[nodiscard]] nlohmann::json
item_json(const std::optional<world::ItemType> item) {
  if (!item) {
    return nullptr;
  }
  return std::string{world::item_name(*item)};
}

[[nodiscard]] std::expected<world::Direction, ToolError>
validate_direction_arguments(const nlohmann::json &arguments,
                             const std::string_view tool) {
  if (!arguments.is_object()) {
    return std::unexpected(
        invalid_arguments(std::string{tool} + " arguments must be an object"));
  }
  if (arguments.size() != 1 || !arguments.contains("direction")) {
    return std::unexpected(invalid_arguments(
        std::string{tool} + " expects exactly one 'direction' property"));
  }
  const auto &value = arguments.at("direction");
  if (!value.is_string()) {
    return std::unexpected(
        invalid_arguments(std::string{tool} + " direction must be a string"));
  }
  const auto parsed = parse_direction(value.get_ref<const std::string &>());
  if (!parsed) {
    return std::unexpected(invalid_arguments(
        std::string{tool} +
        " direction must be one of north, south, east, or west"));
  }
  return *parsed;
}

[[nodiscard]] const nlohmann::json &directional_schema() {
  static const nlohmann::json schema = {
      {"type", "object"},
      {"properties",
       {{"direction",
         {{"type", "string"},
          {"enum", std::array{"north", "south", "east", "west"}}}}}},
      {"required", std::array{"direction"}},
      {"additionalProperties", false},
  };
  return schema;
}

} // namespace

ToolExecutor::ToolExecutor(world::World &world, EventFeed &events,
                           Config config)
    : world_(world), events_(events), config_(std::move(config)) {}

const nlohmann::json &ToolExecutor::move_schema() {
  return directional_schema();
}

const nlohmann::json &ToolExecutor::look_schema() {
  return directional_schema();
}

const nlohmann::json &ToolExecutor::eat_schema() {
  static const nlohmann::json schema = {
      {"type", "object"},
      {"properties", nlohmann::json::object()},
      {"additionalProperties", false},
  };
  return schema;
}

JsonResult ToolExecutor::execute(const std::size_t turn,
                                 const std::string_view tool,
                                 const nlohmann::json &arguments) {
  if (tool == "move" || tool == "look") {
    return execute_directional(turn, tool, arguments);
  }
  if (tool == "eat") {
    return execute_eat(turn, arguments);
  }
  const ToolError error{
      .code = ToolErrorCode::unknown_tool,
      .message = "unknown tool: " + std::string{tool},
  };
  const auto position = world_.position();
  append_event(turn, tool, arguments, error_result(error), position, position);
  return std::unexpected(error);
}

JsonResult ToolExecutor::execute_serialized(const std::size_t turn,
                                            const std::string_view tool,
                                            const std::string_view arguments) {
  const auto parsed = nlohmann::json::parse(arguments, nullptr, false);
  if (parsed.is_discarded()) {
    const ToolError error{
        .code = ToolErrorCode::malformed_json,
        .message = std::string{tool} + " arguments are not valid JSON",
    };
    const auto position = world_.position();
    append_event(turn, tool, {{"_raw", std::string{arguments}}},
                 error_result(error), position, position);
    return std::unexpected(error);
  }
  return execute(turn, tool, parsed);
}

JsonResult ToolExecutor::execute_directional(const std::size_t turn,
                                             const std::string_view tool,
                                             const nlohmann::json &arguments) {
  const auto direction = validate_direction_arguments(arguments, tool);
  if (!direction) {
    const auto position = world_.position();
    append_event(turn, tool, arguments, error_result(direction.error()),
                 position, position);
    return std::unexpected(direction.error());
  }

  const auto before = world_.position();
  nlohmann::json result;
  if (tool == "move") {
    const auto moved = world_.move(*direction);
    if (moved.ok) {
      result = {
          {"ok", true},
          {"position", position_json(moved.position)},
          {"item_here", item_json(moved.item_here)},
      };
    } else {
      result = {
          {"ok", false},
          {"reason", "wall"},
          {"position", position_json(moved.position)},
      };
    }
  } else {
    const auto looked = world_.look(*direction);
    auto cells = nlohmann::json::array();
    for (const auto &cell : looked.cells) {
      nlohmann::json visible_item = nullptr;
      if (cell.item) {
        visible_item = config_.opaque_look ? nlohmann::json("something")
                                           : item_json(cell.item);
      }
      cells.push_back({{"distance", cell.distance}, {"item", visible_item}});
    }
    result = {
        {"direction", std::string{direction_name(looked.direction)}},
        {"cells", std::move(cells)},
        {"wall_at_distance", looked.wall_at_distance},
    };
  }

  append_event(turn, tool, arguments, result, before, world_.position(),
               *direction);
  return result;
}

JsonResult ToolExecutor::execute_eat(const std::size_t turn,
                                     const nlohmann::json &arguments) {
  if (!arguments.is_object() || !arguments.empty()) {
    const auto error =
        invalid_arguments("eat arguments must be an empty object");
    const auto position = world_.position();
    append_event(turn, "eat", arguments, error_result(error), position,
                 position);
    return std::unexpected(error);
  }

  const auto before = world_.position();
  const auto eaten = world_.eat();
  nlohmann::json result;
  if (!eaten.ok) {
    result = {{"ok", false}, {"reason", "nothing_here"}};
  } else {
    result = {
        {"ok", true},
        {"ate", std::string{world::item_name(*eaten.ate)}},
    };
    if (config_.reward_feedback) {
      result["reward"] = eaten.reward;
      result["score"] = eaten.score;
    }
  }

  append_event(turn, "eat", arguments, result, before, world_.position());
  return result;
}

void ToolExecutor::append_event(
    const std::size_t turn, const std::string_view tool,
    const nlohmann::json &arguments, const nlohmann::json &result,
    const world::Position before, const world::Position after,
    const std::optional<world::Direction> direction) {
  events_.push_back(WorldEvent{
      .tick = next_tick_++,
      .turn = turn,
      .tool = std::string{tool},
      .arguments = arguments,
      .result = result,
      .before = before,
      .after = after,
      .direction = direction,
  });
}

} // namespace pigpen::agent
