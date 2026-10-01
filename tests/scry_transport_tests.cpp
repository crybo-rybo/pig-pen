/// @file scry_transport_tests.cpp
/// @brief Exercise Pig Pen's real bindings and transport with Scry's worker,
/// provider codec, dispatch, and history, without sockets or a model server.
#include "agent/scry_transport.hpp"

#include "agent/world_tool_binding.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <scry/testing/scripted_transport.hpp>
#include <scry/testing/streams.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using pigpen::agent::TurnStatus;
using scry::testing::openai_text_stream;
using scry::testing::openai_tool_stream;

scry::Harness make_harness(pigpen::agent::Config config,
                           pigpen::agent::WorldToolBinding &binding,
                           scry::testing::ScriptedTransport &script,
                           const std::size_t max_result_bytes) {
  config.model = "scripted-pig";
  auto registry = binding.registry();
  REQUIRE(registry.has_value());
  auto provider = pigpen::agent::scry_config(config);
  provider.limits.max_tool_result_bytes = max_result_bytes;
  auto created =
      scry::testing::create_harness(provider, script, std::move(*registry));
  REQUIRE(created.has_value());
  return std::move(*created);
}

scry::Conversation make_conversation() {
  auto created = scry::Conversation::create();
  REQUIRE(created.has_value());
  return std::move(*created);
}

template <typename Predicate>
void pump_until(scry::Harness &harness, Predicate done,
                scry::UpdateOptions options = {}) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    harness.update(options);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(done());
}

struct ScriptedWorld {
  explicit ScriptedWorld(pigpen::agent::Config config = {},
                         std::size_t max_result_bytes = 4096)
      : binding(world, config),
        harness(make_harness(config, binding, script, max_result_bytes)),
        conversation(make_conversation()),
        transport(harness, conversation,
                  {.on_tool_request = [this](const scry::ToolRequest &)
                       -> std::optional<scry::ToolRejection> {
                     return binding.admit(logging_failed);
                   },
                   .on_tool_call =
                       [this](const scry::ToolCall &call) {
                         binding.observe(call);
                         calls.push_back(call);
                       },
                   .on_turn_finished =
                       [this] { binding.flush_pending_activity(); }}) {
    binding.on_activity = [this](pigpen::agent::ToolActivity activity) {
      activities.push_back(std::move(activity));
    };
  }

  void enqueue(std::string body) {
    script.enqueue({.body_chunks = {std::move(body)}});
  }

  void send() {
    outcome.reset();
    REQUIRE(transport.send("Continue.", {.on_finished = [this](auto result) {
                             outcome = std::move(result);
                           }}));
  }

  void finish() {
    pump_until(harness, [this] { return outcome.has_value(); });
  }

  pigpen::world::World world{37};
  pigpen::agent::WorldToolBinding binding;
  scry::testing::ScriptedTransport script;
  scry::Harness harness;
  scry::Conversation conversation;
  pigpen::agent::ScryTurnTransport transport;
  std::vector<pigpen::agent::ToolActivity> activities;
  std::vector<scry::ToolCall> calls;
  std::optional<pigpen::agent::TurnOutcome> outcome;
  bool logging_failed{false};
};

} // namespace

TEST_CASE("world tools export their reflected contract without a harness") {
  pigpen::world::World world{37};
  pigpen::agent::WorldToolBinding binding{world, {}};
  auto registry = binding.registry();
  REQUIRE(registry);
  auto manifest = registry->to_json();
  REQUIRE(manifest);
  const auto tools = nlohmann::json::parse(manifest->text).at("tools");
  REQUIRE(tools.size() == 3);
  CHECK(tools[0].at("name") == "move");
  CHECK(tools[1].at("name") == "look");
  CHECK(tools[2].at("name") == "eat");
  CHECK(
      tools[0].at("input_schema") ==
      nlohmann::json::parse(
          scry::reflection::input_schema_v<pigpen::agent::DirectionArguments>));
  CHECK(tools[2].at("input_schema") ==
        nlohmann::json::parse(R"({"additionalProperties":false,)"
                              R"("properties":{},"required":[],)"
                              R"("type":"object"})"));
  CHECK(world.position() == pigpen::world::World::spawn);
}

TEST_CASE("Scry bounds requested calls across batches and resets each turn") {
  ScriptedWorld run;
  run.enqueue(openai_tool_stream({
      {.id = "bad", .name = "move", .arguments = R"({"direction":"up"})"},
      {.id = "first", .name = "move", .arguments = R"({"direction":"east"})"},
  }));
  run.enqueue(openai_tool_stream({
      {.id = "unknown", .name = "fly", .arguments = "{}"},
      {.id = "second", .name = "move", .arguments = R"({"direction":"east"})"},
      {.id = "excess", .name = "move", .arguments = R"({"direction":"east"})"},
      {.id = "excess-look",
       .name = "look",
       .arguments = R"({"direction":"north"})"},
  }));
  run.enqueue(openai_text_stream("Moved twice."));
  run.send();
  run.finish();

  REQUIRE(run.outcome->status == TurnStatus::completed);
  REQUIRE(run.outcome->tool_stats);
  CHECK(run.outcome->tool_stats->rounds == 2);
  CHECK(run.outcome->tool_stats->calls == 6);
  CHECK(run.outcome->tool_stats->rejected_calls == 2);
  CHECK_FALSE(run.outcome->tool_stats->round_limit_reached);
  REQUIRE(run.activities.size() == 2);
  CHECK(run.world.position() == (pigpen::world::Position{7, 5}));
  REQUIRE(run.calls.size() == 6);
  CHECK(run.calls[0].is_error);
  CHECK(run.calls[0].result.text.find("north, south, east, west") !=
        std::string::npos);
  CHECK(run.calls[2].result.text.find("registered tools: eat, look, move") !=
        std::string::npos);
  CHECK(run.calls[4].is_error);
  CHECK(run.calls[5].is_error);
  CHECK(run.calls[4].result.text.find("tool call limit") != std::string::npos);

  const auto &activity = run.activities[1];
  CHECK(activity.call_id == "second");
  CHECK(activity.scry_turn_id == run.calls[3].turn_id.value);
  CHECK(activity.round == 2);
  CHECK(activity.index == 1);
  CHECK(activity.arguments_json == run.calls[3].arguments.text);
  CHECK(activity.result_json == run.calls[3].result.text);
  const auto requests = run.script.requests();
  REQUIRE(requests.size() == 3);
  const auto body = nlohmann::json::parse(requests.back().body);
  bool found = false;
  for (const auto &message : body.at("messages")) {
    if (message.value("tool_call_id", "") == "second") {
      CHECK(message.at("content") == activity.result_json);
      found = true;
    }
  }
  CHECK(found);

  run.enqueue(openai_tool_stream({
      {.id = "new-turn",
       .name = "move",
       .arguments = R"({"direction":"west"})"},
  }));
  run.enqueue(openai_text_stream("Moved west."));
  run.send();
  run.finish();
  REQUIRE(run.outcome->status == TurnStatus::completed);
  REQUIRE(run.outcome->tool_stats);
  CHECK(run.outcome->tool_stats->calls == 1);
  CHECK(run.outcome->tool_stats->rejected_calls == 0);
  REQUIRE(run.activities.size() == 3);
  CHECK(run.activities.back().scry_turn_id !=
        run.activities.front().scry_turn_id);
  CHECK(run.activities.back().round == 1);
  CHECK(run.world.position() == (pigpen::world::Position{6, 5}));
}

TEST_CASE(
    "round limits preserve executed actions and history for the next turn") {
  pigpen::agent::Config config;
  config.max_tool_rounds = 1;
  ScriptedWorld run{config};
  run.enqueue(openai_tool_stream({
      {.id = "executed",
       .name = "move",
       .arguments = R"({"direction":"east"})"},
  }));
  run.enqueue(openai_tool_stream({
      {.id = "dropped-move",
       .name = "move",
       .arguments = R"({"direction":"east"})"},
      {.id = "dropped-eat", .name = "eat", .arguments = "{}"},
  }));
  run.send();
  run.finish();
  REQUIRE(run.outcome->status == TurnStatus::completed);
  REQUIRE(run.outcome->tool_stats);
  CHECK(run.outcome->tool_stats->round_limit_reached);
  CHECK(run.outcome->tool_stats->unexecuted_calls == 2);
  CHECK(run.outcome->tool_stats->calls == 1);
  CHECK(run.outcome->tool_stats->rounds == 1);
  CHECK(run.world.position() == (pigpen::world::Position{6, 5}));
  REQUIRE(run.activities.size() == 1);
  auto history = run.conversation.to_json();
  REQUIRE(history);
  CHECK(history->text.find("executed") != std::string::npos);
  CHECK(history->text.find("dropped-") == std::string::npos);

  run.enqueue(openai_text_stream("Continuing from the actual position."));
  run.send();
  run.finish();
  REQUIRE(run.outcome->status == TurnStatus::completed);
  const auto request = run.script.requests().back().body;
  CHECK(request.find("executed") != std::string::npos);
  CHECK(request.find("dropped-") == std::string::npos);
}

TEST_CASE("logging failure stops a batch after an action without discarding "
          "history") {
  ScriptedWorld run;
  run.binding.on_activity = [&run](pigpen::agent::ToolActivity activity) {
    run.activities.push_back(std::move(activity));
    run.logging_failed = true;
  };
  run.enqueue(openai_tool_stream({
      {.id = "last-action",
       .name = "move",
       .arguments = R"({"direction":"east"})"},
      {.id = "refused", .name = "move", .arguments = R"({"direction":"east"})"},
  }));
  run.enqueue(openai_text_stream("Episode complete."));
  run.send();
  run.finish();
  REQUIRE(run.outcome->status == TurnStatus::completed);
  REQUIRE(run.outcome->tool_stats);
  CHECK(run.outcome->tool_stats->calls == 2);
  CHECK(run.outcome->tool_stats->rejected_calls == 1);
  CHECK(run.world.position() == (pigpen::world::Position{6, 5}));
  REQUIRE(run.activities.size() == 1);
  REQUIRE(run.calls.size() == 2);
  CHECK(run.calls[1].is_error);
  CHECK(run.calls[1].result.text.find("episode log failed") !=
        std::string::npos);
  auto history = run.conversation.to_json();
  REQUIRE(history);
  CHECK(history->text.find("last-action") != std::string::npos);
  CHECK(history->text.find("refused") != std::string::npos);
}

TEST_CASE("eating the final positive item refuses later actions in the batch") {
  ScriptedWorld run;
  std::vector<pigpen::world::Position> positive_cells;
  for (const auto &placement : run.world.items()) {
    if (pigpen::world::item_reward(placement.item) > 0) {
      positive_cells.push_back(placement.position);
    }
  }
  REQUIRE_FALSE(positive_cells.empty());
  using pigpen::world::Direction;
  for (const auto cell : positive_cells) {
    while (run.world.position().x != cell.x) {
      REQUIRE(run.world
                  .move(run.world.position().x < cell.x ? Direction::east
                                                        : Direction::west)
                  .ok);
    }
    while (run.world.position().y != cell.y) {
      REQUIRE(run.world
                  .move(run.world.position().y < cell.y ? Direction::north
                                                        : Direction::south)
                  .ok);
    }
    if (cell != positive_cells.back()) {
      REQUIRE(run.world.eat().ok);
    }
  }
  REQUIRE_FALSE(run.world.all_positive_items_eaten());
  run.enqueue(openai_tool_stream({
      {.id = "last-food", .name = "eat", .arguments = "{}"},
      {.id = "refused", .name = "move", .arguments = R"({"direction":"east"})"},
  }));
  run.enqueue(openai_text_stream("All food collected."));
  run.send();
  run.finish();

  REQUIRE(run.outcome->status == TurnStatus::completed);
  REQUIRE(run.outcome->tool_stats);
  CHECK(run.outcome->tool_stats->calls == 2);
  CHECK(run.outcome->tool_stats->rejected_calls == 1);
  CHECK(run.world.all_positive_items_eaten());
  CHECK(run.world.position() == positive_cells.back());
  REQUIRE(run.activities.size() == 1);
  CHECK(run.activities.front().kind == pigpen::agent::ToolKind::eat);
  REQUIRE(run.calls.size() == 2);
  CHECK_FALSE(run.calls[0].is_error);
  CHECK(run.calls[1].is_error);
  CHECK(run.calls[1].result.text.find(
            "All positive-value items have been eaten") != std::string::npos);
  auto history = run.conversation.to_json();
  REQUIRE(history);
  CHECK(history->text.find("last-food") != std::string::npos);
  CHECK(history->text.find("refused") != std::string::npos);
}

TEST_CASE(
    "Scry transport rejects overlapping sends and delivers cancellation") {
  ScriptedWorld run;
  run.script.enqueue({.hold = true});
  run.send();
  pump_until(run.harness, [&run] { return run.script.calls() == 1; });
  CHECK_FALSE(run.transport.send("overlap", {}));
  REQUIRE(run.transport.cancel());
  run.finish();
  CHECK(run.outcome->status == TurnStatus::cancelled);
  CHECK_FALSE(run.outcome->tool_stats);
  CHECK(run.conversation.messages().empty());
  CHECK_FALSE(run.transport.cancel());
}

TEST_CASE("destroying the Scry transport suppresses late delivery") {
  ScriptedWorld run;
  std::size_t completions{};
  auto transport = std::make_unique<pigpen::agent::ScryTurnTransport>(
      run.harness, run.conversation);
  run.script.enqueue({.hold = true});
  REQUIRE(transport->send(
      "Continue.", {.on_finished = [&completions](auto) { ++completions; }}));
  pump_until(run.harness, [&run] { return run.script.calls() == 1; });
  transport.reset();
  run.script.release();
  pump_until(run.harness, [&run] { return !run.conversation.busy(); });
  CHECK(completions == 0);
  CHECK(run.conversation.messages().empty());
}

TEST_CASE("a turn with an undelivered terminal callback blocks the next "
          "send") {
  ScriptedWorld run;
  std::size_t completions{};
  const auto count = [&completions](auto) { ++completions; };
  auto transport = std::make_unique<pigpen::agent::ScryTurnTransport>(
      run.harness, run.conversation);
  run.enqueue(openai_text_stream("First."));
  run.enqueue(openai_text_stream("Second."));
  REQUIRE(transport->send("One.", {.on_finished = count}));
  // Scry ingests the terminal event and frees the conversation, but a zero
  // callback budget holds on_finished back.
  const auto idle = [&run] { return !run.conversation.busy(); };
  pump_until(run.harness, idle, {.max_callbacks = 0});
  CHECK_FALSE(transport->send("Two.", {.on_finished = count}));

  transport.reset();
  for (int pump = 0; pump < 20; ++pump) {
    run.harness.update();
    std::this_thread::sleep_for(1ms);
  }
  CHECK(completions == 0);
}

TEST_CASE("the provider sampling seed is sent only when configured") {
  const auto sent_seed = [](const std::optional<std::uint32_t> seed) {
    pigpen::agent::Config config;
    config.sampling_seed = seed;
    ScriptedWorld run{config};
    run.enqueue(openai_text_stream("Seeded."));
    run.send();
    run.finish();
    REQUIRE(run.outcome->status == TurnStatus::completed);
    const auto requests = run.script.requests();
    REQUIRE(requests.size() == 1);
    return nlohmann::json::parse(requests.front().body);
  };

  const auto seeded = sent_seed(4'294'967'295U);
  CHECK(seeded.at("seed") == 4'294'967'295U);
  CHECK_FALSE(sent_seed(std::nullopt).contains("seed"));
}

TEST_CASE("truncated Scry completions remain terminal Pig Pen errors") {
  ScriptedWorld run;
  auto body = openai_text_stream("Incomplete.");
  const auto pos = body.find(R"("finish_reason":"stop")");
  REQUIRE(pos != std::string::npos);
  body.replace(pos, std::string{R"("finish_reason":"stop")"}.size(),
               R"("finish_reason":"length")");
  run.enqueue(std::move(body));
  run.send();
  run.finish();
  CHECK(run.outcome->status == TurnStatus::error);
  CHECK(run.outcome->error.find("output-token limit") != std::string::npos);
}

TEST_CASE("world side effects remain observable if Scry cannot post a result") {
  ScriptedWorld run{{}, 1};
  run.enqueue(openai_tool_stream({
      {.id = "oversize",
       .name = "move",
       .arguments = R"({"direction":"east"})"},
  }));
  run.send();
  SECTION("terminal delivery flushes the pending action") {}
  SECTION("shutdown can flush before terminal delivery without duplication") {
    pump_until(
        run.harness,
        [&run] { return run.world.position() != pigpen::world::World::spawn; },
        {.max_callbacks = 1});
    REQUIRE_FALSE(run.outcome);
    REQUIRE(run.activities.empty());
    run.binding.flush_pending_activity();
    REQUIRE(run.activities.size() == 1);
  }
  run.finish();
  CHECK(run.outcome->status == TurnStatus::error);
  CHECK_FALSE(run.outcome->tool_stats);
  CHECK(run.calls.empty());
  CHECK(run.conversation.messages().empty());
  CHECK(run.world.position() == (pigpen::world::Position{6, 5}));
  REQUIRE(run.activities.size() == 1);
  CHECK(run.activities.front().call_id == "oversize");
  CHECK_FALSE(run.activities.front().result_dispatched);
  CHECK(run.activities.front().arguments_json == "null");
  CHECK(run.activities.front().result_json == "null");
  run.binding.flush_pending_activity();
  CHECK(run.activities.size() == 1);
}
