/// @file episode_turn.hpp
/// @brief One retained turn of an episode: the runner's record plus where
/// each of its tool requests ended up.
#pragma once

#include "agent/episode_runner.hpp"

#include <cstdint>
#include <optional>

namespace pigpen::agent {

/// @brief Where each of a turn's tool requests ended up. All four sum to
/// TurnToolStats::calls. Absent when scry produced no Completion.
struct TurnCallTally {
  /// Decoded, admitted, ran a world action (TurnRecord::tool_calls).
  std::uint32_t executed{};
  /// Admitted, but an unknown tool or schema-rejected arguments.
  std::uint32_t invalid{};
  /// Past max_world_tool_calls_per_turn.
  std::uint32_t budget_refused{};
  /// Refused by WorldToolBinding::admit(): objective done or log failed.
  std::uint32_t host_refused{};

  friend bool operator==(const TurnCallTally &,
                         const TurnCallTally &) = default;
};

/// @brief A finished turn as Session retains it.
struct EpisodeTurn {
  TurnRecord record{};
  std::optional<TurnCallTally> calls{};
};

/// @brief Split a turn's Scry call count by outcome.
///
/// Scry counts every request it dispatched in TurnToolStats::calls and the
/// subset refused before a handler ran (the per-turn limit, then the host's
/// admission hook) in TurnToolStats::rejected_calls. Once admitted, unknown
/// tools and undecodable arguments are neither refused nor executed. So:
/// invalid = calls - rejected_calls - executed and
/// budget_refused = rejected_calls - host_refused.
/// @param host_refused_calls Refusals the admission hook returned this turn.
/// @return Empty when @p record has no tool_stats, or when the counts
/// contradict that accounting (never true for a Session's own turns); a tally
/// is therefore never fabricated from inconsistent input.
[[nodiscard]] std::optional<TurnCallTally>
tally_turn_calls(const TurnRecord &record,
                 std::uint32_t host_refused_calls) noexcept;

} // namespace pigpen::agent
