#pragma once

#include "agent/config.hpp"
#include "agent/events.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace pigpen::agent {

enum class ToolErrorCode : std::uint8_t {
  unknown_tool,
  malformed_json,
  invalid_arguments,
};

struct ToolError {
  ToolErrorCode code{ToolErrorCode::invalid_arguments};
  std::string message{};

  friend bool operator==(const ToolError &, const ToolError &) = default;
};

/// Model-independent tool boundary. It owns no world state; it validates the
/// exact public schemas, applies actions synchronously, shapes model-visible
/// JSON, and appends one event for every invocation, including boundary errors.
class ToolExecutor final {
public:
  ToolExecutor(world::World &world, EventFeed &events, Config config = {});

  [[nodiscard]] std::expected<nlohmann::json, ToolError>
  execute(std::size_t turn, std::string_view tool,
          const nlohmann::json &arguments);

  [[nodiscard]] std::expected<nlohmann::json, ToolError>
  execute_serialized(std::size_t turn, std::string_view tool,
                     std::string_view arguments);

  [[nodiscard]] static const nlohmann::json &move_schema();
  [[nodiscard]] static const nlohmann::json &look_schema();
  [[nodiscard]] static const nlohmann::json &eat_schema();

  [[nodiscard]] std::uint64_t next_tick() const noexcept { return next_tick_; }

private:
  [[nodiscard]] std::expected<nlohmann::json, ToolError>
  execute_directional(std::size_t turn, std::string_view tool,
                      const nlohmann::json &arguments);
  [[nodiscard]] std::expected<nlohmann::json, ToolError>
  execute_eat(std::size_t turn, const nlohmann::json &arguments);

  void append_event(std::size_t turn, std::string_view tool,
                    const nlohmann::json &arguments,
                    const nlohmann::json &result, world::Position before,
                    world::Position after,
                    std::optional<world::Direction> direction = {});

  world::World &world_;
  EventFeed &events_;
  Config config_;
  std::uint64_t next_tick_{1};
};

} // namespace pigpen::agent
