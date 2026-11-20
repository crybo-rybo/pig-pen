/// @file tool_loop_tests.cpp
/// @brief Covers ToolLoopTransport with a scripted chat client and a fake
/// dispatcher: request contents, the call budget, admission refusals, the
/// round limit, the committed history, failures, cancellation, and stats.
///
/// No gRPC, reflection, or model: each test delivers the replies of the chat
/// client at the exact moment under test.

#include "core/tool_loop.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pigpen::core;

namespace {

/// @brief Records each request and lets the test deliver its reply.
class ScriptedChat final : public IChatClient {
public:
  std::expected<void, std::string> start(ChatRequest request,
                                         ChatCallbacks callbacks) override {
    if (active) {
      return std::unexpected("a request is already active");
    }
    if (fail_starts_after && requests.size() >= *fail_starts_after) {
      return std::unexpected("could not start the request");
    }
    requests.push_back(std::move(request));
    callbacks_ = std::move(callbacks);
    active = true;
    return {};
  }

  bool cancel() noexcept override {
    ++cancel_count;
    return active;
  }

  void text(const std::string &text) {
    REQUIRE(active);
    callbacks_.on_text(text);
  }

  void finish(ChatFinish finish) { deliver(std::move(finish)); }

  void calls(std::vector<ChatToolCall> calls, ChatStats stats = {}) {
    finish({.reason = ChatFinishReason::tool_calls,
            .stats = stats,
            .tool_calls = std::move(calls)});
  }

  void reply(const std::string &text, ChatStats stats = {}) {
    this->text(text);
    finish({.reason = ChatFinishReason::end_of_generation, .stats = stats});
  }

  void fail(ChatError error) { deliver(std::unexpected(std::move(error))); }

  bool active{false};
  int cancel_count{};
  std::optional<std::size_t> fail_starts_after{};
  std::vector<ChatRequest> requests{};

private:
  void deliver(std::expected<ChatFinish, ChatError> finished) {
    REQUIRE(active);
    active = false;
    auto callbacks = std::exchange(callbacks_, {});
    callbacks.on_finished(std::move(finished));
  }

  ChatCallbacks callbacks_{};
};

/// @brief Two tools; `move` executes, `bad` and unknown names are invalid,
/// and `refuse` makes every call a refusal.
class FakeDispatcher final : public IToolDispatcher {
public:
  struct Dispatched {
    std::uint32_t round{};
    std::uint32_t index{};
    std::string call_id{};
    std::string name{};
    std::string arguments{};
  };

  const std::vector<ChatTool> &tools() const override { return tools_; }

  ToolDispatchResult dispatch(const ToolCallContext &context,
                              const std::string_view name,
                              const std::string_view arguments_json) override {
    dispatched.push_back({context.round, context.index,
                          std::string{context.call_id}, std::string{name},
                          std::string{arguments_json}});
    if (on_dispatch) {
      on_dispatch();
    }
    if (refuse) {
      return {.status = ToolDispatchStatus::refused,
              .content = R"({"error":"refused"})"};
    }
    if (name != "move") {
      return {.status = ToolDispatchStatus::invalid,
              .content = R"({"error":"invalid )" + std::string{name} + "\"}"};
    }
    return {.status = ToolDispatchStatus::executed,
            .content =
                R"({"moved":)" + std::to_string(dispatched.size()) + "}"};
  }

  bool refuse{false};
  /// Runs inside each dispatch(), before the result is made.
  std::function<void()> on_dispatch{};
  std::vector<Dispatched> dispatched{};

private:
  std::vector<ChatTool> tools_{
      {.name = "move", .description = "Move.", .parameters_json_schema = "{}"},
      {.name = "look", .description = "Look.", .parameters_json_schema = "{}"},
  };
};

struct Loop {
  explicit Loop(ToolLoopLimits limits = {},
                ChatSampling sampling = {.temperature = 0.5,
                                         .max_tokens = 128,
                                         .seed = 9})
      : transport(chat, dispatcher, "SYSTEM", sampling, limits) {}

  void send(const std::string &message = "Continue.") {
    outcome.reset();
    REQUIRE(transport.send(
        message,
        {.on_text_delta = [this](auto text) { deltas.emplace_back(text); },
         .on_finished =
             [this](TurnOutcome finished) { outcome = std::move(finished); }}));
  }

  ScriptedChat chat;
  FakeDispatcher dispatcher;
  ToolLoopTransport transport;
  std::vector<std::string> deltas{};
  std::optional<TurnOutcome> outcome{};
};

ChatToolCall call(std::string id, std::string name = "move",
                  std::string arguments = R"({"direction":"east"})") {
  return {.id = std::move(id),
          .name = std::move(name),
          .arguments_json = std::move(arguments)};
}

/// @brief Whether any message of @p messages mentions @p text.
bool mentions(const std::vector<ChatMessage> &messages,
              const std::string &text) {
  for (const auto &message : messages) {
    if (message.content.find(text) != std::string::npos ||
        message.tool_call_id == text) {
      return true;
    }
    for (const auto &tool_call : message.tool_calls) {
      if (tool_call.id == text) {
        return true;
      }
    }
  }
  return false;
}

/// @brief The roles of @p messages, in order.
std::vector<ChatRole> roles(const std::vector<ChatMessage> &messages) {
  std::vector<ChatRole> result;
  for (const auto &message : messages) {
    result.push_back(message.role);
  }
  return result;
}

/// @brief Whether every call in @p messages has a `tool` message answer.
bool all_calls_answered(const std::vector<ChatMessage> &messages) {
  return std::ranges::all_of(messages, [&](const ChatMessage &message) {
    return std::ranges::all_of(
        message.tool_calls, [&](const ChatToolCall &tool_call) {
          return std::ranges::any_of(messages, [&](const ChatMessage &answer) {
            return answer.role == ChatRole::tool &&
                   answer.tool_call_id == tool_call.id;
          });
        });
  });
}

/// @brief The committed history after one answered round with call `c1`.
std::vector<ChatMessage> one_answered_round(const std::string &user) {
  return {
      {.role = ChatRole::system, .content = "SYSTEM"},
      {.role = ChatRole::user, .content = user},
      {.role = ChatRole::assistant, .tool_calls = {call("c1")}},
      {.role = ChatRole::tool,
       .content = R"({"moved":1})",
       .tool_call_id = "c1"},
  };
}

} // namespace

TEST_CASE("each round resends the history with the tools and the sampling") {
  Loop loop;
  loop.send("Turn one.");
  REQUIRE(loop.chat.requests.size() == 1);
  // A copy: the next round appends to `requests`, which can reallocate it.
  const auto first = loop.chat.requests[0];
  REQUIRE(first.messages.size() == 2);
  CHECK(first.messages[0] ==
        ChatMessage{.role = ChatRole::system, .content = "SYSTEM"});
  CHECK(first.messages[1] ==
        ChatMessage{.role = ChatRole::user, .content = "Turn one."});
  CHECK(first.tools == loop.dispatcher.tools());
  CHECK(first.sampling ==
        ChatSampling{.temperature = 0.5, .max_tokens = 128, .seed = 9});

  loop.chat.text("Looking. ");
  loop.chat.calls({call("c1"), call("c2", "look", R"({"direction":"north"})")});
  REQUIRE(loop.chat.requests.size() == 2);
  const auto &second = loop.chat.requests[1];
  REQUIRE(second.messages.size() == 5);
  CHECK(second.messages[2] ==
        ChatMessage{
            .role = ChatRole::assistant,
            .content = "Looking. ",
            .tool_calls = {call("c1"),
                           call("c2", "look", R"({"direction":"north"})")}});
  CHECK(second.messages[3] == ChatMessage{.role = ChatRole::tool,
                                          .content = R"({"moved":1})",
                                          .tool_call_id = "c1"});
  CHECK(second.messages[4] ==
        ChatMessage{.role = ChatRole::tool,
                    .content = R"({"error":"invalid look"})",
                    .tool_call_id = "c2"});
  CHECK(second.tools == loop.dispatcher.tools());
  CHECK(second.sampling == first.sampling);
  REQUIRE(loop.dispatcher.dispatched.size() == 2);
  CHECK(loop.dispatcher.dispatched[0].round == 1);
  CHECK(loop.dispatcher.dispatched[0].index == 0);
  CHECK(loop.dispatcher.dispatched[0].call_id == "c1");
  CHECK(loop.dispatcher.dispatched[0].arguments == R"({"direction":"east"})");
  CHECK(loop.dispatcher.dispatched[1].index == 1);
  CHECK(loop.dispatcher.dispatched[1].name == "look");

  loop.chat.reply("Done.");
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.outcome->text == "Looking. Done.");
  CHECK(loop.deltas == std::vector<std::string>{"Looking. ", "Done."});
  CHECK(loop.outcome->requests == 2);
  // The committed history is the system prompt plus the whole turn.
  REQUIRE(loop.transport.history().size() == 6);
  CHECK(loop.transport.history().back() ==
        ChatMessage{.role = ChatRole::assistant, .content = "Done."});

  loop.send("Turn two.");
  REQUIRE(loop.chat.requests.size() == 3);
  CHECK(loop.chat.requests[2].messages.size() == 7);
  CHECK(loop.chat.requests[2].messages.back().content == "Turn two.");
}

TEST_CASE("an unset sampling seed stays unset on every request") {
  Loop loop{{}, {.temperature = 0.0, .max_tokens = 64}};
  loop.send();
  loop.chat.calls({call("c1")});
  loop.chat.reply("Done.");
  REQUIRE(loop.chat.requests.size() == 2);
  for (const auto &request : loop.chat.requests) {
    CHECK_FALSE(request.sampling.seed.has_value());
    CHECK(request.sampling.max_tokens == 64);
  }
}

TEST_CASE("the call budget spans batches, counts invalid calls, and resets "
          "each turn") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("bad", "bad", R"({"direction":"up"})"), call("first")});
  loop.chat.calls({call("unknown", "fly", "{}"), call("second"), call("excess"),
                   call("excess-look", "look", R"({"direction":"north"})")});
  loop.chat.reply("Moved twice.");

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.outcome->requests == 3);
  CHECK(loop.outcome->tool_stats ==
        TurnToolStats{
            .rounds = 2, .calls = 6, .rejected_calls = 2, .invalid_calls = 2});
  // Invalid calls spend the budget; the over-budget calls never dispatch.
  REQUIRE(loop.dispatcher.dispatched.size() == 4);
  CHECK(loop.dispatcher.dispatched[3].call_id == "second");
  CHECK(loop.dispatcher.dispatched[3].round == 2);
  CHECK(loop.dispatcher.dispatched[3].index == 1);
  const auto &last = loop.chat.requests.back().messages;
  REQUIRE(last.size() >= 4);
  CHECK(last[last.size() - 2] ==
        ChatMessage{.role = ChatRole::tool,
                    .content = std::string{tool_call_limit_result},
                    .tool_call_id = "excess"});
  CHECK(last.back() ==
        ChatMessage{.role = ChatRole::tool,
                    .content = std::string{tool_call_limit_result},
                    .tool_call_id = "excess-look"});
  CHECK(
      tool_call_limit_result ==
      R"({"error":"tool call limit for this turn reached; respond without calling tools"})");

  loop.send();
  loop.chat.calls({call("new-turn")});
  loop.chat.reply("Moved again.");
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->tool_stats ==
        TurnToolStats{.rounds = 1, .calls = 1, .rejected_calls = 0});
  REQUIRE(loop.dispatcher.dispatched.size() == 5);
  CHECK(loop.dispatcher.dispatched.back().round == 1);
}

TEST_CASE("admission refusals are rejected calls whose answers stay in the "
          "history") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("allowed")});
  loop.dispatcher.refuse = true;
  loop.chat.calls({call("refused")});
  loop.chat.reply("Episode complete.");
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.outcome->tool_stats ==
        TurnToolStats{.rounds = 2, .calls = 2, .rejected_calls = 1});
  const auto &history = loop.transport.history();
  REQUIRE(history.size() == 7);
  CHECK(history[3] == ChatMessage{.role = ChatRole::tool,
                                  .content = R"({"moved":1})",
                                  .tool_call_id = "allowed"});
  CHECK(history[5] == ChatMessage{.role = ChatRole::tool,
                                  .content = R"({"error":"refused"})",
                                  .tool_call_id = "refused"});
}

TEST_CASE("the round limit completes the turn without dangling calls") {
  Loop loop{{.max_tool_rounds = 1}};
  loop.send();
  loop.chat.calls({call("executed")});
  loop.chat.text("I will continue east.");
  loop.chat.calls({call("dropped-move"), call("dropped-eat", "eat", "{}")});

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.outcome->requests == 2);
  CHECK(loop.outcome->tool_stats == TurnToolStats{.rounds = 1,
                                                  .calls = 1,
                                                  .round_limit_reached = true,
                                                  .unexecuted_calls = 2});
  REQUIRE(loop.dispatcher.dispatched.size() == 1);
  const auto &history = loop.transport.history();
  CHECK(mentions(history, "executed"));
  CHECK_FALSE(mentions(history, "dropped-move"));
  CHECK_FALSE(mentions(history, "dropped-eat"));
  CHECK(history.back() == ChatMessage{.role = ChatRole::assistant,
                                      .content = "I will continue east."});
  CHECK(all_calls_answered(history));

  loop.send();
  loop.chat.reply("Continuing from the actual position.");
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->tool_stats == TurnToolStats{});
  CHECK(mentions(loop.chat.requests.back().messages, "executed"));
  CHECK_FALSE(mentions(loop.chat.requests.back().messages, "dropped-move"));
}

TEST_CASE("a round-limit reply without text commits an empty assistant "
          "message") {
  Loop loop{{.max_tool_rounds = 1}};
  loop.send();
  loop.chat.calls({call("executed")});
  loop.chat.calls({call("dropped")});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.outcome->tool_stats.unexecuted_calls == 1);
  const auto &history = loop.transport.history();
  CHECK(roles(history) == std::vector{ChatRole::system, ChatRole::user,
                                      ChatRole::assistant, ChatRole::tool,
                                      ChatRole::assistant});
  CHECK(history.back() == ChatMessage{.role = ChatRole::assistant});
  CHECK(all_calls_answered(history));
  CHECK_FALSE(mentions(history, "dropped"));
}

TEST_CASE("an empty final reply commits an empty assistant message, so the "
          "next request alternates roles") {
  Loop loop;
  loop.send("Turn one.");
  loop.chat.finish({.reason = ChatFinishReason::end_of_generation});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.outcome->text.empty());
  CHECK(loop.transport.history().back() ==
        ChatMessage{.role = ChatRole::assistant});

  loop.send("Turn two.");
  const auto &messages = loop.chat.requests.back().messages;
  CHECK(roles(messages) == std::vector{ChatRole::system, ChatRole::user,
                                       ChatRole::assistant, ChatRole::user});
  CHECK(messages.back().content == "Turn two.");
}

TEST_CASE("an output-token limit before a tool round is a turn error that "
          "names the limit and commits nothing") {
  Loop loop{{.context_tokens = 4096}};
  loop.send();
  loop.chat.text("Incomplete.");
  loop.chat.finish({.reason = ChatFinishReason::length,
                    .stats = {.prompt_tokens = 10, .completion_tokens = 128}});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.outcome->error == "model response reached its output-token limit "
                               "of 128 tokens after 128 output tokens");
  CHECK(loop.outcome->requests == 1);
  CHECK(loop.outcome->output_tokens == 128);
  CHECK(loop.transport.history().size() == 1);
}

TEST_CASE("a length finish at a full context ends the turn "
          "context_exhausted") {
  Loop loop{{.context_tokens = 2816}};
  loop.send();
  loop.chat.text("I");
  SECTION("the counts fill the context exactly") {
    loop.chat.finish(
        {.reason = ChatFinishReason::length,
         .stats = {.prompt_tokens = 2815, .completion_tokens = 1}});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->error ==
          "the model context is full: 2815 prompt tokens and 1 output tokens "
          "fill the context of 2816 tokens");
  }
  SECTION("the counts go past the context") {
    loop.chat.finish(
        {.reason = ChatFinishReason::length,
         .stats = {.prompt_tokens = 2815, .completion_tokens = 2}});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->error ==
          "the model context is full: 2815 prompt tokens and 2 output tokens "
          "fill the context of 2816 tokens");
  }
  CHECK(loop.outcome->status == TurnStatus::context_exhausted);
  CHECK(loop.outcome->requests == 1);
  CHECK(loop.outcome->input_tokens == 2815);
  // No round was answered, so the history is unchanged.
  CHECK(loop.transport.history().size() == 1);
}

TEST_CASE("a length finish one token below the context is an output-limit "
          "error") {
  Loop loop{{.context_tokens = 2816}};
  loop.send();
  loop.chat.finish(
      {.reason = ChatFinishReason::length,
       .stats = {.prompt_tokens = 2687, .completion_tokens = 128}});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.outcome->error == "model response reached its output-token limit "
                               "of 128 tokens after 128 output tokens");
}

TEST_CASE("a length finish with an unknown context is an error that names "
          "both causes") {
  Loop loop;
  loop.send();
  loop.chat.finish({.reason = ChatFinishReason::length,
                    .stats = {.prompt_tokens = 4094, .completion_tokens = 2}});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.outcome->error ==
        "model response stopped at a length limit after 2 output tokens: "
        "either the output-token limit of 128 tokens, or a full context (4094 "
        "prompt tokens; the daemon did not report its context size)");
  CHECK(loop.transport.history().size() == 1);
}

TEST_CASE("the kind of a client error decides the turn status") {
  Loop loop{{.context_tokens = 4096}};
  loop.send();
  SECTION("a failure is an error") {
    loop.chat.fail({.message = "llamad INTERNAL: decode failed"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::error);
    CHECK(loop.outcome->error == "llamad INTERNAL: decode failed");
  }
  SECTION("a cancellation is cancelled") {
    REQUIRE(loop.transport.cancel());
    loop.chat.fail({.kind = ChatErrorKind::cancelled,
                    .message = "the model request was cancelled"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::cancelled);
  }
  SECTION("a prompt that is too long exhausts the context, with the "
          "daemon's message") {
    loop.chat.fail({.kind = ChatErrorKind::prompt_too_long,
                    .message = "llamad OUT_OF_RANGE: prompt is too long: 4140 "
                               "tokens for a context of 4096"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::context_exhausted);
    CHECK(loop.outcome->error == "llamad OUT_OF_RANGE: prompt is too long: "
                                 "4140 tokens for a context of 4096");
  }
  SECTION("a cancel wins over a prompt that is too long") {
    REQUIRE(loop.transport.cancel());
    loop.chat.fail({.kind = ChatErrorKind::prompt_too_long,
                    .message = "llamad OUT_OF_RANGE: prompt is too long"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::cancelled);
  }
  CHECK(loop.outcome->requests == 1);
  CHECK(loop.transport.history().size() == 1);
}

TEST_CASE("a stop string completes the turn like the end of generation") {
  Loop loop;
  loop.send();
  loop.chat.text("Stopped.");
  loop.chat.finish({.reason = ChatFinishReason::stop});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  CHECK(loop.transport.history().back().content == "Stopped.");
}

TEST_CASE("a failure after a tool round keeps the side effects and commits "
          "the answered round") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("c1")}, {.prompt_tokens = 100, .completion_tokens = 5});
  loop.chat.text("Unanswered reply.");
  loop.chat.fail({.message = "llamad INTERNAL: decode failed"});

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.outcome->error == "llamad INTERNAL: decode failed");
  CHECK(loop.outcome->requests == 2);
  CHECK(loop.outcome->input_tokens == 100);
  CHECK(loop.outcome->tool_stats ==
        TurnToolStats{.rounds = 1, .calls = 1, .rejected_calls = 0});
  REQUIRE(loop.dispatcher.dispatched.size() == 1);
  // The round ran, so the history keeps it; the failed reply is discarded.
  CHECK(loop.transport.history() == one_answered_round("Continue."));

  // The next turn starts from the answered round.
  loop.send("Again.");
  auto expected = one_answered_round("Continue.");
  expected.push_back({.role = ChatRole::user, .content = "Again."});
  CHECK(loop.chat.requests.back().messages == expected);
}

TEST_CASE("a turn that ends early after one answered round commits exactly "
          "that round") {
  Loop loop{{.context_tokens = 4096}};
  loop.send();
  loop.chat.calls({call("c1")});
  REQUIRE(loop.chat.active);
  loop.chat.text("Unanswered reply.");
  auto expected_status = TurnStatus::error;

  SECTION("a client error") {
    loop.chat.fail({.message = "llamad INTERNAL: decode failed"});
  }
  SECTION("a length finish below the context") {
    loop.chat.finish({.reason = ChatFinishReason::length});
  }
  SECTION("a length finish at a full context") {
    loop.chat.finish(
        {.reason = ChatFinishReason::length,
         .stats = {.prompt_tokens = 4000, .completion_tokens = 96}});
    expected_status = TurnStatus::context_exhausted;
  }
  SECTION("a prompt that is too long") {
    loop.chat.fail({.kind = ChatErrorKind::prompt_too_long,
                    .message = "llamad OUT_OF_RANGE: prompt is too long"});
    expected_status = TurnStatus::context_exhausted;
  }
  SECTION("a cancellation") {
    REQUIRE(loop.transport.cancel());
    loop.chat.fail({.kind = ChatErrorKind::cancelled, .message = "cancelled"});
    expected_status = TurnStatus::cancelled;
  }
  SECTION("a cancel that races a second tool-call reply") {
    REQUIRE(loop.transport.cancel());
    loop.chat.calls({call("late")});
    expected_status = TurnStatus::cancelled;
  }

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == expected_status);
  CHECK(loop.outcome->requests == 2);
  CHECK(loop.dispatcher.dispatched.size() == 1);
  CHECK(loop.transport.history() == one_answered_round("Continue."));
  CHECK(all_calls_answered(loop.transport.history()));

  loop.send("Again.");
  auto expected = one_answered_round("Continue.");
  expected.push_back({.role = ChatRole::user, .content = "Again."});
  CHECK(loop.chat.requests.back().messages == expected);
}

TEST_CASE("a failure before any round was answered leaves the history "
          "unchanged") {
  Loop loop;
  loop.send("Turn one.");
  loop.chat.reply("Done.");
  REQUIRE(loop.outcome);
  const auto before = loop.transport.history();
  REQUIRE(before.size() == 3);

  loop.send("Turn two.");
  loop.chat.text("Partial.");
  SECTION("a client error") {
    loop.chat.fail({.message = "llamad INTERNAL: decode failed"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::error);
  }
  SECTION("a length finish") {
    loop.chat.finish({.reason = ChatFinishReason::length});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::error);
  }
  SECTION("a prompt that is too long") {
    loop.chat.fail({.kind = ChatErrorKind::prompt_too_long,
                    .message = "llamad OUT_OF_RANGE: prompt is too long"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::context_exhausted);
  }
  SECTION("a cancellation") {
    REQUIRE(loop.transport.cancel());
    loop.chat.fail({.kind = ChatErrorKind::cancelled, .message = "cancelled"});
    REQUIRE(loop.outcome);
    CHECK(loop.outcome->status == TurnStatus::cancelled);
  }
  // No lone `user` message: the failed turn left nothing.
  CHECK(loop.transport.history() == before);
}

TEST_CASE("a later round that cannot start ends the turn through on_finished") {
  Loop loop;
  loop.chat.fail_starts_after = 1;
  loop.send();
  loop.chat.calls({call("c1")});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.outcome->error == "could not start the request");
  CHECK(loop.outcome->requests == 1);
  // The round ran before the next request failed to start, so it commits.
  CHECK(loop.transport.history() == one_answered_round("Continue."));
  CHECK_FALSE(loop.transport.turn_active());
}

TEST_CASE("a first request that cannot start is returned by send") {
  Loop loop;
  loop.chat.fail_starts_after = 0;
  std::size_t finished{};
  CHECK_FALSE(loop.transport.send(
      "Continue.", {.on_finished = [&finished](auto) { ++finished; }}));
  CHECK(finished == 0);
  CHECK_FALSE(loop.transport.turn_active());
  CHECK(loop.transport.history().size() == 1);
  loop.chat.fail_starts_after.reset();
  loop.send();
  CHECK(loop.chat.requests.back().messages.size() == 2);
}

TEST_CASE("overlapping sends are rejected") {
  Loop loop;
  loop.send();
  CHECK_FALSE(loop.transport.send("overlap", {}));
  loop.chat.reply("Done.");
  REQUIRE(loop.outcome);
  CHECK(loop.chat.requests.size() == 1);
}

TEST_CASE("cancellation mid-round ends the turn cancelled and commits the "
          "answered round") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("c1")});
  loop.chat.text("Partial ");
  REQUIRE(loop.transport.cancel());
  CHECK(loop.chat.cancel_count == 1);
  CHECK_FALSE(loop.transport.cancel());
  loop.chat.fail({.kind = ChatErrorKind::cancelled, .message = "cancelled"});

  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::cancelled);
  CHECK(loop.outcome->requests == 2);
  CHECK(loop.outcome->text == "Partial ");
  CHECK(loop.outcome->tool_stats.calls == 1);
  CHECK(loop.transport.history() == one_answered_round("Continue."));
  const auto forwarded = loop.chat.cancel_count;
  // Without a turn, cancel() does nothing.
  CHECK_FALSE(loop.transport.cancel());
  CHECK(loop.chat.cancel_count == forwarded);
}

TEST_CASE("a second cancel during a request reaches the client again and "
          "returns false") {
  Loop loop;
  loop.send();
  REQUIRE(loop.transport.cancel());
  CHECK(loop.chat.cancel_count == 1);
  CHECK_FALSE(loop.transport.cancel());
  CHECK(loop.chat.cancel_count == 2);
  loop.chat.fail({.kind = ChatErrorKind::cancelled, .message = "cancelled"});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::cancelled);
}

TEST_CASE("a cancel from inside dispatch finishes the round and starts no "
          "request") {
  Loop loop;
  std::optional<bool> accepted;
  loop.dispatcher.on_dispatch = [&loop, &accepted] {
    if (!accepted) {
      accepted = loop.transport.cancel();
    }
  };
  loop.send();
  loop.chat.calls({call("c1"), call("c2")});

  CHECK(accepted == true);
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::cancelled);
  CHECK(loop.outcome->requests == 1);
  CHECK(loop.chat.requests.size() == 1);
  CHECK_FALSE(loop.chat.active);
  CHECK_FALSE(loop.transport.turn_active());
  // The round finishes, so each call has its answer, and it commits.
  CHECK(loop.dispatcher.dispatched.size() == 2);
  CHECK(loop.outcome->tool_stats.rounds == 1);
  const auto &history = loop.transport.history();
  CHECK(roles(history) == std::vector{ChatRole::system, ChatRole::user,
                                      ChatRole::assistant, ChatRole::tool,
                                      ChatRole::tool});
  CHECK(all_calls_answered(history));
}

TEST_CASE("a cancel followed by a client error ends the turn cancelled") {
  Loop loop;
  loop.send();
  REQUIRE(loop.transport.cancel());
  loop.chat.fail({.message = "llamad UNAVAILABLE: connection reset"});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::cancelled);
  CHECK(loop.outcome->error == "llamad UNAVAILABLE: connection reset");
  CHECK(loop.transport.history().size() == 1);
}

TEST_CASE("a cancel that loses the race to a final reply completes and "
          "commits") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("c1")});
  REQUIRE(loop.transport.cancel());
  loop.chat.reply("Done.");
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::completed);
  auto expected = one_answered_round("Continue.");
  expected.push_back({.role = ChatRole::assistant, .content = "Done."});
  CHECK(loop.transport.history() == expected);
}

TEST_CASE("a cancel that races a tool-call reply runs no tools") {
  Loop loop;
  loop.send();
  REQUIRE(loop.transport.cancel());
  loop.chat.calls({call("late")});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::cancelled);
  CHECK(loop.dispatcher.dispatched.empty());
  CHECK(loop.chat.requests.size() == 1);
}

TEST_CASE("a tool-call finish without calls ends the turn with an error") {
  Loop loop;
  loop.send();
  loop.chat.finish({.reason = ChatFinishReason::tool_calls});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.transport.history().size() == 1);
}

TEST_CASE("token counts accumulate across the rounds of a turn") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("c1")}, {.prompt_tokens = 100,
                                 .completion_tokens = 10,
                                 .cached_prompt_tokens = 0});
  loop.chat.reply("Done.", {.prompt_tokens = 150,
                            .completion_tokens = 20,
                            .cached_prompt_tokens = 95});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->input_tokens == 250);
  CHECK(loop.outcome->output_tokens == 30);
  CHECK(loop.outcome->cached_input_tokens == 95);
  CHECK(loop.outcome->requests == 2);
}

TEST_CASE("model timings sum across the rounds of a turn") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("c1")}, {.prompt_ms = 120.25, .completion_ms = 40.5});
  loop.chat.calls({call("c2")}, {.prompt_ms = 10.5, .completion_ms = 30.0});
  loop.chat.reply("Done.", {.prompt_ms = 5.0, .completion_ms = 12.25});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->requests == 3);
  CHECK(loop.outcome->model_prompt_ms == 135.75);
  CHECK(loop.outcome->model_completion_ms == 82.75);

  // A failed turn keeps the timings of the requests that finished.
  loop.send();
  loop.chat.calls({call("c3")}, {.prompt_ms = 7.5, .completion_ms = 2.5});
  loop.chat.fail({.message = "llamad INTERNAL: decode failed"});
  REQUIRE(loop.outcome);
  CHECK(loop.outcome->status == TurnStatus::error);
  CHECK(loop.outcome->model_prompt_ms == 7.5);
  CHECK(loop.outcome->model_completion_ms == 2.5);
}

TEST_CASE("every request of a turn asks for no reasoning") {
  Loop loop;
  loop.send();
  loop.chat.calls({call("c1")});
  loop.chat.reply("Done.");
  REQUIRE(loop.chat.requests.size() == 2);
  for (const auto &request : loop.chat.requests) {
    CHECK(request.reasoning == ChatReasoning::off);
  }
}

TEST_CASE("destroying the tool loop cancels and ignores late delivery") {
  ScriptedChat chat;
  FakeDispatcher dispatcher;
  std::size_t finished{};
  {
    ToolLoopTransport transport{chat, dispatcher, "SYSTEM", {}, {}};
    REQUIRE(transport.send("Continue.",
                           {.on_finished = [&finished](auto) { ++finished; }}));
  }
  CHECK(chat.cancel_count == 1);
  chat.calls({call("late")});
  CHECK(finished == 0);
  CHECK(dispatcher.dispatched.empty());
}
