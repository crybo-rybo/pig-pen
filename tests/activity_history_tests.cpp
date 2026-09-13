#include "ui/activity_history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace {

[[nodiscard]] pigpen::agent::ToolActivity
activity(const std::size_t turn, const pigpen::agent::ToolKind kind,
         std::string arguments, std::string result,
         const std::optional<pigpen::world::ItemType> eaten = std::nullopt) {
  return {
      .tick = turn * 10U,
      .turn = turn,
      .kind = kind,
      .arguments_json = std::move(arguments),
      .result_json = std::move(result),
      .eaten = eaten,
  };
}

} // namespace

TEST_CASE("activity history incrementally indexes turns and statistics",
          "[ui][history]") {
  pigpen::agent::ToolActivityFeed activities{
      activity(1U, pigpen::agent::ToolKind::look, R"({"direction":"north"})",
               R"({"status":"ok"})"),
      activity(1U, pigpen::agent::ToolKind::move, R"({"direction":"north"})",
               R"({"status":"ok"})"),
  };
  pigpen::ui::ActivityHistory history;
  history.synchronize(activities);

  CHECK(history.indexed_size() == 2U);
  const auto first_turn = pigpen::ui::ActivityRange{.begin = 0U, .end = 2U};
  CHECK(history.range_for_turn(1U) == std::optional{first_turn});
  CHECK_FALSE(history.range_for_turn(2U));
  CHECK(history.counts().looks == 1U);
  CHECK(history.counts().moves == 1U);

  activities.push_back(activity(2U, pigpen::agent::ToolKind::eat, "{}",
                                R"({"status":"nothing_to_eat"})"));
  activities.push_back(activity(2U, pigpen::agent::ToolKind::eat, "{}",
                                R"({"status":"ok"})",
                                pigpen::world::ItemType::apple));
  history.synchronize(activities);

  const auto second_turn = pigpen::ui::ActivityRange{.begin = 2U, .end = 4U};
  CHECK(history.range_for_turn(2U) == std::optional{second_turn});
  CHECK(history.counts().eat_attempts == 2U);
  CHECK(history.counts().successful_eats == 1U);
  CHECK(history.counts().failed_eats == 1U);
}

TEST_CASE("activity filter normalizes once and handles appended records",
          "[ui][history]") {
  pigpen::agent::ToolActivityFeed activities{
      activity(1U, pigpen::agent::ToolKind::look, R"({"direction":"WEST"})",
               R"({"status":"ok"})"),
      activity(2U, pigpen::agent::ToolKind::move, R"({"direction":"east"})",
               R"({"status":"blocked_by_wall"})"),
  };
  pigpen::ui::ActivityHistory history;
  history.synchronize(activities);

  CHECK(history.set_filter("  WeSt ", activities));
  REQUIRE(history.filtered_indices().size() == 1U);
  CHECK(history.filtered_indices().front() == 0U);
  CHECK_FALSE(history.set_filter("west", activities));

  activities.push_back(activity(3U, pigpen::agent::ToolKind::move,
                                R"({"direction":"west"})",
                                R"({"status":"ok"})"));
  history.synchronize(activities);
  REQUIRE(history.filtered_indices().size() == 2U);
  CHECK(history.filtered_indices().back() == 2U);
}

TEST_CASE("activity history reset retains filter for a replacement session",
          "[ui][history]") {
  pigpen::ui::ActivityHistory history;
  pigpen::agent::ToolActivityFeed first{
      activity(1U, pigpen::agent::ToolKind::look, "{}", "apple")};
  history.synchronize(first);
  REQUIRE(history.set_filter("apple", first));
  REQUIRE(history.filtered_indices().size() == 1U);

  history.reset();
  pigpen::agent::ToolActivityFeed replacement{
      activity(1U, pigpen::agent::ToolKind::eat, "{}", "toadstool"),
      activity(2U, pigpen::agent::ToolKind::eat, "{}", "apple"),
  };
  history.synchronize(replacement);

  CHECK(history.indexed_size() == 2U);
  REQUIRE(history.filtered_indices().size() == 1U);
  CHECK(history.filtered_indices().front() == 1U);
  CHECK(history.counts().eat_attempts == 2U);
}

TEST_CASE("changing an activity filter indexes pending records exactly once",
          "[ui][history]") {
  pigpen::ui::ActivityHistory history;
  pigpen::agent::ToolActivityFeed activities{
      activity(1U, pigpen::agent::ToolKind::look, "{}", "apple")};
  REQUIRE(history.set_filter("apple", activities));
  REQUIRE(history.filtered_indices().size() == 1U);
  CHECK(history.presentation(0U).short_result == "apple");

  activities.push_back(
      activity(2U, pigpen::agent::ToolKind::look, "{}", "apple"));
  CHECK_FALSE(history.set_filter(" APPLE ", activities));
  history.synchronize(activities);

  REQUIRE(history.filtered_indices().size() == 2U);
  CHECK(history.filtered_indices()[0] == 0U);
  CHECK(history.filtered_indices()[1] == 1U);
  CHECK(history.counts().looks == 2U);
}
