/// @file episode_turn.cpp
/// @brief Turn call tally derivation; the contract is in the header.
#include "agent/episode_turn.hpp"

namespace pigpen::agent {

std::optional<TurnCallTally>
tally_turn_calls(const TurnRecord &record,
                 const std::uint32_t host_refused_calls) noexcept {
  if (!record.tool_stats) {
    return std::nullopt;
  }
  const auto &stats = *record.tool_stats;
  // Checked in this order, each subtraction below is non-negative.
  if (stats.rejected_calls > stats.calls ||
      host_refused_calls > stats.rejected_calls ||
      record.tool_calls > stats.calls - stats.rejected_calls) {
    return std::nullopt;
  }
  const auto executed = static_cast<std::uint32_t>(record.tool_calls);
  return TurnCallTally{
      .executed = executed,
      .invalid = stats.calls - stats.rejected_calls - executed,
      .budget_refused = stats.rejected_calls - host_refused_calls,
      .host_refused = host_refused_calls,
  };
}

} // namespace pigpen::agent
