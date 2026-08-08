#pragma once

#include "world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// One successfully decoded world-tool handler invocation and its observable
/// world transition. Application-budget rejections carry a structured error
/// response and identical before/after positions.
struct WorldEvent {
  std::uint64_t tick{};
  std::size_t turn{};
  std::string tool{};
  nlohmann::json arguments{nlohmann::json::object()};
  nlohmann::json result{nlohmann::json::object()};
  world::Position before{};
  world::Position after{};
  std::optional<world::Direction> direction{};
  bool action_executed{};
  std::optional<world::ItemType> eaten{};

  friend bool operator==(const WorldEvent &, const WorldEvent &) = default;
};

using EventFeed = std::vector<WorldEvent>;

} // namespace pigpen::agent
