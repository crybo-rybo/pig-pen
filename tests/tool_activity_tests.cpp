/// @file tool_activity_tests.cpp
/// @brief Covers the typed ToolActivity record and the journal that owns it:
/// tick stamping, derived counters, persistence forwarding, and the latched
/// failure the session consumes to fail an episode outside tool dispatch.

#include "agent/tool_activity.hpp"

#include "agent/tool_activity_journal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using pigpen::agent::ToolActivity;
using pigpen::agent::ToolActivityJournal;
using pigpen::agent::ToolKind;
using pigpen::agent::ToolOutcome;
using pigpen::world::ItemType;

[[nodiscard]] ToolActivity move_activity(const std::size_t turn) {
  return {
      .turn = turn,
      .kind = ToolKind::move,
      .outcome = ToolOutcome::succeeded,
      .before = {.x = 5, .y = 5},
      .after = {.x = 6, .y = 5},
      .direction = pigpen::world::Direction::east,
      .summary = "move east: (5, 5) -> (6, 5)",
  };
}

} // namespace

TEST_CASE("tool activity distinguishes execution from success") {
  ToolActivity activity = move_activity(1);
  CHECK(activity.action_executed());
  CHECK(activity.succeeded());

  activity.outcome = ToolOutcome::blocked_by_wall;
  CHECK(activity.action_executed());
  CHECK_FALSE(activity.succeeded());

  activity.outcome = ToolOutcome::nothing_to_eat;
  CHECK(activity.action_executed());
  CHECK_FALSE(activity.succeeded());

  activity.outcome = ToolOutcome::budget_exhausted;
  CHECK_FALSE(activity.action_executed());
  CHECK_FALSE(activity.succeeded());
}

TEST_CASE("tool kind and outcome names are stable lowercase identifiers") {
  CHECK(pigpen::agent::tool_kind_name(ToolKind::move) == "move");
  CHECK(pigpen::agent::tool_kind_name(ToolKind::look) == "look");
  CHECK(pigpen::agent::tool_kind_name(ToolKind::eat) == "eat");
  CHECK(pigpen::agent::tool_outcome_name(ToolOutcome::succeeded) ==
        "succeeded");
  CHECK(pigpen::agent::tool_outcome_name(ToolOutcome::blocked_by_wall) ==
        "blocked_by_wall");
  CHECK(pigpen::agent::tool_outcome_name(ToolOutcome::nothing_to_eat) ==
        "nothing_to_eat");
  CHECK(pigpen::agent::tool_outcome_name(ToolOutcome::budget_exhausted) ==
        "budget_exhausted");
}

TEST_CASE("journal stamps monotonic ticks and maintains counters") {
  ToolActivityJournal journal;
  journal.publish(move_activity(1));
  journal.publish({
      .turn = 1,
      .kind = ToolKind::look,
      .outcome = ToolOutcome::succeeded,
      .direction = pigpen::world::Direction::north,
  });
  journal.publish({
      .turn = 2,
      .kind = ToolKind::eat,
      .outcome = ToolOutcome::succeeded,
      .eaten = ItemType::berry,
      .score_after = 1,
  });
  journal.publish({
      .turn = 2,
      .kind = ToolKind::eat,
      .outcome = ToolOutcome::nothing_to_eat,
      .score_after = 1,
  });

  const auto &activities = journal.activities();
  REQUIRE(activities.size() == 4);
  CHECK(activities[0].tick == 1);
  CHECK(activities[1].tick == 2);
  CHECK(activities[2].tick == 3);
  CHECK(activities[3].tick == 4);

  CHECK(journal.call_count() == 4);
  CHECK(journal.call_count(ToolKind::move) == 1);
  CHECK(journal.call_count(ToolKind::look) == 1);
  CHECK(journal.call_count(ToolKind::eat) == 2);
  CHECK(journal.eaten_count(ItemType::berry) == 1);
  CHECK(journal.eaten_count(ItemType::truffle) == 0);
  CHECK(journal.eaten_total() == 1);
  CHECK(journal.persistence_error().empty());
  CHECK_FALSE(journal.take_persistence_failure());
}

TEST_CASE("journal forwards every activity to the persistence hook") {
  std::vector<ToolActivity> persisted;
  ToolActivityJournal journal{[&persisted](const ToolActivity &activity)
                                  -> std::expected<void, std::string> {
    persisted.push_back(activity);
    return {};
  }};

  journal.publish(move_activity(1));
  journal.publish(move_activity(2));

  REQUIRE(persisted.size() == 2);
  // The persisted record is the committed one, tick included.
  CHECK(persisted[0] == journal.activities()[0]);
  CHECK(persisted[1] == journal.activities()[1]);
  CHECK(persisted[0].tick == 1);
}

TEST_CASE("persistence failure is latched once and never blocks the record") {
  std::size_t persist_calls = 0;
  ToolActivityJournal journal{[&persist_calls](const ToolActivity &)
                                  -> std::expected<void, std::string> {
    ++persist_calls;
    return std::unexpected("disk full");
  }};

  journal.publish(move_activity(1));
  // The activity committed even though persistence failed.
  CHECK(journal.call_count() == 1);
  CHECK(journal.persistence_error() == "disk full");
  CHECK(journal.take_persistence_failure());
  // The latch is consumed exactly once.
  CHECK_FALSE(journal.take_persistence_failure());

  // A terminal persistence failure stops further writer calls but not
  // further activity commits.
  journal.publish(move_activity(2));
  CHECK(journal.call_count() == 2);
  CHECK(persist_calls == 1);
  CHECK_FALSE(journal.take_persistence_failure());
  CHECK(journal.persistence_error() == "disk full");
}
