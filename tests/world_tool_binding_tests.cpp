/// @file world_tool_binding_tests.cpp
/// @brief Covers the reflected world tools: the exact manifest, the encoder,
/// decode errors, unknown tools, exact activity payloads and call identity,
/// and admission after objective completion and after a log failure, also
/// inside one batch driven by the real tool loop.
///
/// These TUs compile as C++26 with reflection, because they include the
/// scrydantic toolbox. No gRPC or model: a scripted chat client drives the
/// tool loop.

#include "agent/world_tool_binding.hpp"

#include "core/tool_loop.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using pigpen::agent::DirectionArguments;
using pigpen::agent::WorldToolBinding;
using pigpen::agent::WorldToolDispatcher;
using pigpen::core::ToolDispatchStatus;

namespace {

constexpr std::string_view direction_schema =
    R"({"additionalProperties":false,"properties":{"direction":{"description":"Cardinal direction: north, south, east, or west","enum":["north","south","east","west"],"type":"string"}},"required":["direction"],"type":"object"})";
constexpr std::string_view eat_schema =
    R"({"additionalProperties":false,"properties":{},"required":[],"type":"object"})";

struct Tools {
  explicit Tools(std::uint64_t seed = 37)
      : world(seed), binding(std::make_shared<WorldToolBinding>(
                         world, pigpen::core::Config{})),
        dispatcher(binding, [this] { return logging_failed; }) {
    dispatcher.on_activity = [this](pigpen::core::ToolActivity activity) {
      activities.push_back(std::move(activity));
      if (fail_log_on_activity) {
        logging_failed = true;
      }
    };
  }

  pigpen::core::ToolDispatchResult dispatch(const std::string_view name,
                                            const std::string_view arguments,
                                            const std::string_view id = "call",
                                            const std::uint32_t round = 1,
                                            const std::uint32_t index = 0) {
    return dispatcher.dispatch({.round = round, .index = index, .call_id = id},
                               name, arguments);
  }

  /// @brief Walk to every positive item and eat all but the last one.
  void eat_all_but_last_positive_item() {
    std::vector<pigpen::world::Position> positive_cells;
    for (const auto &placement : world.items()) {
      if (pigpen::world::item_reward(placement.item) > 0) {
        positive_cells.push_back(placement.position);
      }
    }
    REQUIRE_FALSE(positive_cells.empty());
    using pigpen::world::Direction;
    for (const auto cell : positive_cells) {
      while (world.position().x != cell.x) {
        REQUIRE(world
                    .move(world.position().x < cell.x ? Direction::east
                                                      : Direction::west)
                    .ok);
      }
      while (world.position().y != cell.y) {
        REQUIRE(world
                    .move(world.position().y < cell.y ? Direction::north
                                                      : Direction::south)
                    .ok);
      }
      if (cell != positive_cells.back()) {
        REQUIRE(world.eat().ok);
      }
    }
    REQUIRE_FALSE(world.all_positive_items_eaten());
  }

  pigpen::world::World world;
  std::shared_ptr<WorldToolBinding> binding;
  WorldToolDispatcher dispatcher;
  std::vector<pigpen::core::ToolActivity> activities{};
  bool logging_failed{false};
  bool fail_log_on_activity{false};
};

/// @brief Records requests; the test delivers each reply.
class ScriptedChat final : public pigpen::core::IChatClient {
public:
  std::expected<void, std::string>
  start(pigpen::core::ChatRequest request,
        pigpen::core::ChatCallbacks callbacks) override {
    requests.push_back(std::move(request));
    callbacks_ = std::move(callbacks);
    return {};
  }
  bool cancel() noexcept override { return false; }

  void calls(std::vector<pigpen::core::ChatToolCall> calls) {
    std::exchange(callbacks_, {})
        .on_finished(pigpen::core::ChatFinish{
            .reason = pigpen::core::ChatFinishReason::tool_calls,
            .tool_calls = std::move(calls)});
  }
  void reply(const std::string &text) {
    auto callbacks = std::exchange(callbacks_, {});
    callbacks.on_text(text);
    callbacks.on_finished(pigpen::core::ChatFinish{});
  }
  void fail(std::string message) {
    std::exchange(callbacks_, {})
        .on_finished(std::unexpected(
            pigpen::core::ChatError{.message = std::move(message)}));
  }

  std::vector<pigpen::core::ChatRequest> requests{};

private:
  pigpen::core::ChatCallbacks callbacks_{};
};

/// @brief The real tool loop over the real world dispatcher.
struct Loop {
  Loop()
      : transport(chat, tools.dispatcher, "SYSTEM", {},
                  {.max_tool_rounds = 8, .max_tool_calls_per_turn = 4}) {}

  void send() {
    outcome.reset();
    REQUIRE(transport.send("Continue.", {.on_finished = [this](auto result) {
                             outcome = std::move(result);
                           }}));
  }

  /// @brief The tool message that answers @p id in the last request.
  [[nodiscard]] std::string answer(const std::string &id) const {
    for (const auto &message : chat.requests.back().messages) {
      if (message.role == pigpen::core::ChatRole::tool &&
          message.tool_call_id == id) {
        return message.content;
      }
    }
    FAIL("no answer for " << id);
    return {};
  }

  Tools tools;
  ScriptedChat chat;
  pigpen::core::ToolLoopTransport transport;
  std::optional<pigpen::core::TurnOutcome> outcome{};
};

pigpen::core::ChatToolCall call(std::string id, std::string name,
                                std::string arguments) {
  return {.id = std::move(id),
          .name = std::move(name),
          .arguments_json = std::move(arguments)};
}

} // namespace

static_assert(scrydantic::input_schema_v<DirectionArguments> ==
              direction_schema);

TEST_CASE("scrydantic encodes typed arguments and responses canonically") {
  pigpen::world::World world{9};
  pigpen::core::WorldTools tools{world};
  const auto moved = tools.move(pigpen::world::Direction::east);
  const auto arguments = scrydantic::encode(
      DirectionArguments{.direction = pigpen::world::Direction::east});
  const auto response = scrydantic::encode(moved);

  REQUIRE(arguments.has_value());
  REQUIRE(response.has_value());
  CHECK(arguments->text == R"({"direction":"east"})");
  // World seed 9 has an apple east of the spawn cell.
  CHECK(
      response->text ==
      R"({"item_here":"apple","ok":true,"position":{"x":6,"y":5},"reason":null})");
}

TEST_CASE("the world tools export their exact reflected contract") {
  Tools run;
  const auto &tools = run.dispatcher.tools();
  REQUIRE(tools.size() == 3);
  CHECK(tools[0] ==
        pigpen::core::ChatTool{
            .name = "move",
            .description = "Move one cell north, south, east, or west.",
            .parameters_json_schema = std::string{direction_schema}});
  CHECK(tools[1] ==
        pigpen::core::ChatTool{
            .name = "look",
            .description = "Scan every cell in one direction to the wall.",
            .parameters_json_schema = std::string{direction_schema}});
  CHECK(tools[2] ==
        pigpen::core::ChatTool{
            .name = "eat",
            .description = "Eat the item on the current cell, if present.",
            .parameters_json_schema = std::string{eat_schema}});
  CHECK(run.world.position() == pigpen::world::World::spawn);
  CHECK(run.activities.empty());
}

TEST_CASE("a decoded call publishes its exact payloads and identity") {
  Tools run;
  const auto result =
      run.dispatch("move", R"({ "direction" : "east" })", "call-7", 2, 1);
  CHECK(result.status == ToolDispatchStatus::executed);
  CHECK(
      result.content ==
      R"({"item_here":null,"ok":true,"position":{"x":6,"y":5},"reason":null})");
  REQUIRE(run.activities.size() == 1);
  const auto &activity = run.activities.front();
  CHECK(activity.kind == pigpen::core::ToolKind::move);
  CHECK(activity.outcome == pigpen::core::ToolOutcome::succeeded);
  CHECK(activity.call_id == "call-7");
  CHECK(activity.round == 2);
  CHECK(activity.index == 1);
  // The canonical arguments, and the exact text that went to the model.
  CHECK(activity.arguments_json == R"({"direction":"east"})");
  CHECK(activity.result_json == result.content);
  CHECK(activity.before == pigpen::world::World::spawn);
  CHECK(activity.after == (pigpen::world::Position{6, 5}));
  CHECK(activity.direction == pigpen::world::Direction::east);

  const auto eaten = run.dispatch("eat", "{}");
  CHECK(eaten.status == ToolDispatchStatus::executed);
  REQUIRE(run.activities.size() == 2);
  CHECK(run.activities.back().arguments_json == "{}");
  CHECK(run.activities.back().result_json == eaten.content);
  const auto looked = run.dispatch("look", R"({"direction":"north"})");
  REQUIRE(run.activities.size() == 3);
  CHECK(nlohmann::json::parse(looked.content).at("direction") == "north");
}

TEST_CASE("a wall is a typed outcome, not an error") {
  Tools run;
  for (int step = 0; step < 4; ++step) {
    REQUIRE(run.dispatch("move", R"({"direction":"north"})").status ==
            ToolDispatchStatus::executed);
  }
  const auto blocked = run.dispatch("move", R"({"direction":"north"})");
  CHECK(blocked.status == ToolDispatchStatus::executed);
  CHECK(nlohmann::json::parse(blocked.content) ==
        nlohmann::json{{"item_here", nullptr},
                       {"ok", false},
                       {"position", {{"x", 5}, {"y", 9}}},
                       {"reason", "wall"}});
  CHECK(run.activities.back().outcome ==
        pigpen::core::ToolOutcome::blocked_by_wall);
}

TEST_CASE("undecodable arguments and unknown tools are invalid and never "
          "act") {
  Tools run;
  const auto bad = run.dispatch("move", R"({"direction":"up"})");
  CHECK(bad.status == ToolDispatchStatus::invalid);
  CHECK(
      bad.content ==
      R"({"error":"$.direction is not a declared enumerator; must be one of: north, south, east, west"})");

  const auto extra = run.dispatch("move", R"({"direction":"east","speed":2})");
  CHECK(extra.status == ToolDispatchStatus::invalid);
  CHECK(nlohmann::json::parse(extra.content).at("error") ==
        R"($ contains unknown member "speed")");

  const auto eat_arguments = run.dispatch("eat", R"({"what":"berry"})");
  CHECK(eat_arguments.status == ToolDispatchStatus::invalid);

  const auto not_json = run.dispatch("look", "{not json");
  CHECK(not_json.status == ToolDispatchStatus::invalid);
  CHECK(nlohmann::json::parse(not_json.content).contains("error"));

  const auto unknown = run.dispatch("fly", "{}");
  CHECK(unknown.status == ToolDispatchStatus::invalid);
  CHECK(
      unknown.content ==
      R"({"error":"unknown tool \"fly\"; registered tools: eat, look, move"})");
  // A made-up name is escaped, never spliced.
  const auto quoted = run.dispatch(R"(x"y)", "{}");
  CHECK(nlohmann::json::parse(quoted.content).at("error") ==
        R"(unknown tool "x"y"; registered tools: eat, look, move)");

  CHECK(run.activities.empty());
  CHECK(run.world.position() == pigpen::world::World::spawn);
}

TEST_CASE("a log failure refuses world actions") {
  Tools run;
  run.logging_failed = true;
  const auto refused = run.dispatch("move", R"({"direction":"east"})");
  CHECK(refused.status == ToolDispatchStatus::refused);
  CHECK(nlohmann::json::parse(refused.content).at("error") ==
        "World tools are unavailable because the episode log failed. "
        "Summarize the actions already taken.");
  CHECK(run.activities.empty());
  CHECK(run.world.position() == pigpen::world::World::spawn);
}

TEST_CASE("a log failure stops a batch after an action without discarding "
          "history") {
  Loop loop;
  loop.tools.fail_log_on_activity = true;
  loop.send();
  loop.chat.calls({call("last-action", "move", R"({"direction":"east"})"),
                   call("refused", "move", R"({"direction":"east"})")});
  CHECK(nlohmann::json::parse(loop.answer("refused")).at("error") ==
        "World tools are unavailable because the episode log failed. "
        "Summarize the actions already taken.");
  loop.chat.reply("Episode complete.");

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == pigpen::core::TurnStatus::completed);
  CHECK(loop.outcome->tool_stats.calls == 2);
  CHECK(loop.outcome->tool_stats.rejected_calls == 1);
  CHECK(loop.tools.world.position() == (pigpen::world::Position{6, 5}));
  REQUIRE(loop.tools.activities.size() == 1);
  const auto &history = loop.transport.history();
  CHECK(std::ranges::count(history, std::string{"last-action"},
                           &pigpen::core::ChatMessage::tool_call_id) == 1);
  CHECK(std::ranges::count(history, std::string{"refused"},
                           &pigpen::core::ChatMessage::tool_call_id) == 1);
}

TEST_CASE("eating the final positive item refuses later actions in the "
          "batch") {
  Loop loop;
  loop.tools.eat_all_but_last_positive_item();
  const auto last_cell = loop.tools.world.position();
  loop.send();
  loop.chat.calls({call("last-food", "eat", "{}"),
                   call("refused", "move", R"({"direction":"east"})")});
  CHECK(nlohmann::json::parse(loop.answer("last-food")).at("ok") == true);
  CHECK(nlohmann::json::parse(loop.answer("refused")).at("error") ==
        "All positive-value items have been eaten. Summarize the completed "
        "episode without more tools.");
  loop.chat.reply("All food collected.");

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == pigpen::core::TurnStatus::completed);
  CHECK(loop.outcome->tool_stats.calls == 2);
  CHECK(loop.outcome->tool_stats.rejected_calls == 1);
  CHECK(loop.tools.world.all_positive_items_eaten());
  CHECK(loop.tools.world.position() == last_cell);
  REQUIRE(loop.tools.activities.size() == 1);
  CHECK(loop.tools.activities.front().kind == pigpen::core::ToolKind::eat);
}

TEST_CASE("the tool message is the logged result, and side effects survive "
          "a failed turn") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("bad", "move", R"({"direction":"up"})"),
                   call("first", "move", R"({"direction":"east"})")});
  REQUIRE(loop.tools.activities.size() == 1);
  CHECK(loop.tools.activities[0].call_id == "first");
  CHECK(loop.tools.activities[0].index == 1);
  CHECK(loop.tools.activities[0].result_json == loop.answer("first"));
  CHECK(
      loop.answer("bad") ==
      R"({"error":"$.direction is not a declared enumerator; must be one of: north, south, east, west"})");
  loop.chat.fail("llamad INTERNAL: decode failed");

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == pigpen::core::TurnStatus::error);
  CHECK(loop.outcome->tool_stats.invalid_calls == 1);
  // The world action happened and was published once; the history did not
  // keep the failed turn.
  CHECK(loop.tools.world.position() == (pigpen::world::Position{6, 5}));
  CHECK(loop.tools.activities.size() == 1);
  CHECK(loop.transport.history().size() == 1);
}
