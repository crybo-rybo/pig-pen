/// @file session_options.hpp
/// @brief Host plumbing for a Session: where (and whether) it logs, how it
/// labels and authenticates its requests, and how its reward is weighted.
///
/// Config is the episode contract: what the model sees and how the world
/// behaves. SessionOptions is everything else a host decides. A front end
/// expresses its mode entirely through these options and through owning the
/// pump loop; nothing here changes what the model is told.
#pragma once

#include "agent/reward.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pigpen::agent {

/// @brief Request header that carries SessionOptions::rollout_id. Pig Pen
/// manages it, so a request header of the same name is rejected.
inline constexpr std::string_view rollout_header_name{"X-Pigpen-Rollout"};

/// @brief How a host runs a Session, as opposed to what the episode is.
struct SessionOptions {
  /// Directory for the episode's JSONL log; absent means no log is opened
  /// at all. The facts a log would record still live on the Session.
  std::optional<std::filesystem::path> log_directory{};
  /// Free-form label recorded in the log header.
  std::string prompt_variant{"default"};
  /// Provider credential; empty for an unauthenticated local server. Front
  /// ends read it from the environment; the Session never does.
  std::string api_key{};
  /// Identifies this episode to the model server. When non-empty it is sent
  /// as the rollout_header_name request header on every request and recorded
  /// in the log header.
  std::string rollout_id{};
  /// Extra `{name, value}` headers sent verbatim on every request. Names that
  /// collide with a provider-managed header, or with rollout_header_name,
  /// make Session::create fail before a log is opened.
  std::vector<std::pair<std::string, std::string>> request_headers{};
  /// Weights the summary and log footer compute the reward with.
  RewardWeights reward_weights{};
};

} // namespace pigpen::agent
