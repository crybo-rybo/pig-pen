/// @file events.hpp
/// @brief The append-only feed of decoded world-tool invocations.
///
/// The UI, the animation, and the metrics writer all read the same feed, so
/// one successfully decoded handler invocation produces exactly one record
/// everywhere.
#pragma once

#include "world/world.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// @brief One successfully decoded world-tool handler invocation and its
/// observable world transition.
/// @note Application-budget rejections still appear here: they carry a
/// structured error response, `action_executed == false`, and identical
/// before/after positions.
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

/// @brief Append-only event history; indices are stable for the lifetime of
/// a session.
using EventFeed = std::vector<WorldEvent>;

} // namespace pigpen::agent
