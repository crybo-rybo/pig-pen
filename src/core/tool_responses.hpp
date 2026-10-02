/// @file tool_responses.hpp
/// @brief The typed results of the world tools, as the model receives them.
///
/// These are plain C++23 aggregates: no annotation is needed to describe them,
/// so they live outside the reflection layer. Scry's codec reflects over them
/// in tool_contract.hpp, and their member and enumerator names are the JSON the
/// model sees; renaming one changes the model-facing contract.
#pragma once

#include "world/world.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pigpen::core {

/// @brief World result for an admitted `move` call.
struct MoveToolResponse {
  bool ok{};
  std::optional<world::ItemType> item_here{};
  world::Position position{};
  std::optional<world::MoveFailure> reason{};
};

/// @brief One scanned cell as shown to the model; `item` may read
/// "something" in opaque-look episodes.
struct LookToolCell {
  int distance{};
  std::optional<std::string> item{};
};

/// @brief Response envelope for `look`.
struct LookToolResponse {
  bool ok{};
  world::Direction direction{};
  std::vector<LookToolCell> cells{};
  int wall_at_distance{};
};

/// @brief Response envelope for `eat`; `reward` and `score` are withheld in
/// no-reward-feedback episodes.
struct EatToolResponse {
  bool ok{};
  std::optional<world::ItemType> ate{};
  std::optional<world::EatFailure> reason{};
  std::optional<int> reward{};
  std::optional<int> score{};
};

} // namespace pigpen::core
