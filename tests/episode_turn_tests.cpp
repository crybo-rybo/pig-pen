/// @file episode_turn_tests.cpp
/// @brief Covers the turn call tally arithmetic on plain records: the split
/// of Scry's call count, absence without a Completion, and refusal to
/// fabricate a tally from counts that contradict Scry's accounting.

#include "agent/episode_turn.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

using pigpen::agent::tally_turn_calls;
using pigpen::agent::TurnCallTally;
using pigpen::agent::TurnRecord;
using pigpen::agent::TurnToolStats;

[[nodiscard]] TurnRecord record(const std::size_t executed,
                                const std::uint32_t calls,
                                const std::uint32_t rejected_calls) {
  return {
      .tool_calls = executed,
      .tool_stats = TurnToolStats{.rounds = 1,
                                  .calls = calls,
                                  .rejected_calls = rejected_calls},
  };
}

[[nodiscard]] std::uint32_t sum(const TurnCallTally &tally) {
  return tally.executed + tally.invalid + tally.budget_refused +
         tally.host_refused;
}

} // namespace

TEST_CASE("the call tally splits Scry's call count by outcome", "[tally]") {
  // Two executed, one invalid, two over the limit, one refused by the host.
  const auto tally = tally_turn_calls(record(2, 6, 3), 1);
  REQUIRE(tally);
  CHECK(*tally == TurnCallTally{.executed = 2,
                                .invalid = 1,
                                .budget_refused = 2,
                                .host_refused = 1});
  CHECK(sum(*tally) == 6);
}

TEST_CASE("the call tally covers empty and single-outcome turns", "[tally]") {
  CHECK(tally_turn_calls(record(0, 0, 0), 0) == TurnCallTally{});
  CHECK(tally_turn_calls(record(4, 4, 0), 0) == TurnCallTally{.executed = 4});
  CHECK(tally_turn_calls(record(0, 3, 0), 0) == TurnCallTally{.invalid = 3});
  CHECK(tally_turn_calls(record(0, 2, 2), 2) ==
        TurnCallTally{.host_refused = 2});
  CHECK(tally_turn_calls(record(0, 5, 1), 0) ==
        TurnCallTally{.invalid = 4, .budget_refused = 1});
}

TEST_CASE("the call tally is absent without Scry statistics", "[tally]") {
  TurnRecord failed{.tool_calls = 1};
  CHECK_FALSE(tally_turn_calls(failed, 0));
  CHECK_FALSE(tally_turn_calls(failed, 1));
}

TEST_CASE("the call tally refuses counts that contradict Scry's accounting",
          "[tally]") {
  // More refusals than calls.
  CHECK_FALSE(tally_turn_calls(record(0, 1, 2), 0));
  // More host refusals than Scry counted as rejected.
  CHECK_FALSE(tally_turn_calls(record(0, 3, 1), 2));
  // More executed actions than admitted calls.
  CHECK_FALSE(tally_turn_calls(record(3, 4, 2), 0));
  CHECK_FALSE(tally_turn_calls(record(5, 4, 0), 0));
  // The boundary where every admitted call executed is consistent.
  CHECK(tally_turn_calls(record(2, 4, 2), 1) ==
        TurnCallTally{.executed = 2, .budget_refused = 1, .host_refused = 1});
}
