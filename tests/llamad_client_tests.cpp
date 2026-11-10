/// @file llamad_client_tests.cpp
/// @brief Covers LlamadChatClient against the scripted fake llamad over a
/// real gRPC Unix socket: the wire mapping both ways, stream order, failure
/// statuses, cancellation, connect rejections, and delivery after
/// destruction.

#include "llamad/llamad_client.hpp"

#include "support/fake_llamad.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using pigpen::core::ChatError;
using pigpen::core::ChatFinish;
using pigpen::core::ChatFinishReason;
using pigpen::core::ChatRole;
using pigpen::llamad::LlamadChatClient;
using pigpen::testing::FakeFinishReason;
using pigpen::testing::FakeLlamad;
using pigpen::testing::FakeReply;

namespace {

using namespace std::chrono_literals;

using Finished = std::expected<ChatFinish, ChatError>;

struct Harness {
  explicit Harness(pigpen::testing::FakeModelInfo info = {})
      : fake(FakeLlamad::start(directory.socket(), std::move(info))) {
    REQUIRE(fake);
    auto connected = LlamadChatClient::connect(directory.socket());
    REQUIRE(connected.has_value());
    client = std::move(*connected);
  }

  /// @brief Start one request and record what it delivers, in order.
  void start(pigpen::core::ChatRequest request = basic_request()) {
    finished.reset();
    events.clear();
    REQUIRE(client->start(
        std::move(request),
        {.on_text = [this](auto text) { events.emplace_back(text); },
         .on_finished =
             [this](Finished result) {
               events.emplace_back("<finished>");
               finished = std::move(result);
             }}));
  }

  void finish() {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
      static_cast<void>(client->pump());
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(finished);
  }

  [[nodiscard]] static pigpen::core::ChatRequest basic_request() {
    return {.messages = {{.role = ChatRole::user, .content = "Hello."}}};
  }

  pigpen::testing::TempSocketDirectory directory;
  std::unique_ptr<FakeLlamad> fake;
  std::unique_ptr<LlamadChatClient> client;
  std::vector<std::string> events{};
  std::optional<Finished> finished{};
};

} // namespace

TEST_CASE("connect reports the model that the daemon serves") {
  Harness run{{.description = "qwen2 0.5B Q4_K - Medium",
               .n_params = 494'000'000,
               .size_bytes = 400'000'000,
               .n_ctx = 8192,
               .n_ctx_train = 32768}};
  CHECK(run.client->model_info() ==
        pigpen::core::ModelInfo{.description = "qwen2 0.5B Q4_K - Medium",
                                .n_params = 494'000'000,
                                .size_bytes = 400'000'000,
                                .n_ctx = 8192,
                                .n_ctx_train = 32768});
  CHECK(run.client->socket_path() == run.directory.socket());
}

TEST_CASE("requests map one to one onto the llamad wire") {
  Harness run;
  run.fake->enqueue(FakeReply::say("ok"));
  run.start({
      .messages =
          {
              {.role = ChatRole::system, .content = "SYSTEM"},
              {.role = ChatRole::user, .content = "Go."},
              {.role = ChatRole::assistant,
               .content = "Moving.",
               .tool_calls = {{.id = "c1",
                               .name = "move",
                               .arguments_json = R"({"direction":"east"})"}}},
              {.role = ChatRole::tool,
               .content = R"({"ok":true})",
               .tool_call_id = "c1"},
          },
      .tools = {{.name = "move",
                 .description = "Move one cell.",
                 .parameters_json_schema = R"({"type":"object"})"}},
      .sampling = {.temperature = 0.25,
                   .max_tokens = 512,
                   .seed = 4'294'967'295U},
  });
  run.finish();
  REQUIRE(run.finished->has_value());

  const auto requests = run.fake->requests();
  REQUIRE(requests.size() == 1);
  const auto &request = requests.front();
  REQUIRE(request.messages.size() == 4);
  CHECK(request.messages[0].role == "system");
  CHECK(request.messages[0].content == "SYSTEM");
  CHECK(request.messages[1].role == "user");
  CHECK(request.messages[2].role == "assistant");
  CHECK(request.messages[2].content == "Moving.");
  REQUIRE(request.messages[2].tool_calls.size() == 1);
  CHECK(request.messages[2].tool_calls[0] ==
        pigpen::testing::FakeToolCall{.id = "c1",
                                      .name = "move",
                                      .arguments_json =
                                          R"({"direction":"east"})"});
  CHECK(request.messages[3].role == "tool");
  CHECK(request.messages[3].tool_call_id == "c1");
  CHECK(request.messages[3].content == R"({"ok":true})");
  REQUIRE(request.tools.size() == 1);
  CHECK(request.tools[0].name == "move");
  CHECK(request.tools[0].description == "Move one cell.");
  CHECK(request.tools[0].parameters_json_schema == R"({"type":"object"})");
  CHECK(request.temperature == 0.25F);
  CHECK(request.max_tokens == 512);
  CHECK(request.seed == 4'294'967'295U);
  CHECK_FALSE(request.top_k);
  CHECK(request.response_json_schema.empty());
}

TEST_CASE("an unset sampling seed is not sent") {
  Harness run;
  run.fake->enqueue(FakeReply::say("ok"));
  run.start();
  run.finish();
  const auto requests = run.fake->requests();
  REQUIRE(requests.size() == 1);
  CHECK_FALSE(requests.front().seed);
  CHECK(requests.front().temperature == 0.0F);
  CHECK(requests.front().max_tokens == 8192);
}

TEST_CASE("text streams in order before the finish with calls and stats") {
  Harness run;
  run.fake->enqueue(
      {.text = {"I ", "look ", "east."},
       .finish = FakeFinishReason::tool_calls,
       .tool_calls = {{.id = "a",
                       .name = "look",
                       .arguments_json = R"({"direction":"east"})"},
                      {.id = "b", .name = "eat"}},
       .prompt_tokens = 120,
       .completion_tokens = 17,
       .cached_prompt_tokens = 96});
  run.start();
  bool delivered_inside_start = !run.events.empty();
  CHECK_FALSE(delivered_inside_start);
  run.finish();
  CHECK(run.events ==
        std::vector<std::string>{"I ", "look ", "east.", "<finished>"});
  REQUIRE(run.finished->has_value());
  const auto &finish = **run.finished;
  CHECK(finish.reason == ChatFinishReason::tool_calls);
  CHECK(finish.stats.prompt_tokens == 120);
  CHECK(finish.stats.completion_tokens == 17);
  CHECK(finish.stats.cached_prompt_tokens == 96);
  REQUIRE(finish.tool_calls.size() == 2);
  CHECK(
      finish.tool_calls[0] ==
      pigpen::core::ChatToolCall{.id = "a",
                                 .name = "look",
                                 .arguments_json = R"({"direction":"east"})"});
  CHECK(finish.tool_calls[1].arguments_json == "{}");
}

TEST_CASE("every finish reason maps to its plain value") {
  Harness run;
  const std::vector<std::pair<FakeFinishReason, ChatFinishReason>> reasons{
      {FakeFinishReason::eog, ChatFinishReason::end_of_generation},
      {FakeFinishReason::length, ChatFinishReason::length},
      {FakeFinishReason::stop, ChatFinishReason::stop},
      {FakeFinishReason::cancelled, ChatFinishReason::cancelled},
  };
  for (const auto &[wire, plain] : reasons) {
    run.fake->enqueue({.finish = wire});
    run.start();
    run.finish();
    REQUIRE(run.finished->has_value());
    CHECK((*run.finished)->reason == plain);
  }
}

TEST_CASE("failure statuses become errors with the status name") {
  Harness run;
  const auto failure = [&run](FakeReply reply) {
    run.fake->enqueue(std::move(reply));
    run.start();
    run.finish();
    REQUIRE_FALSE(run.finished->has_value());
    CHECK_FALSE(run.finished->error().cancelled);
    return run.finished->error().message;
  };

  CHECK(failure({.status_code = 3, .status_message = "prompt is too long"}) ==
        "llamad INVALID_ARGUMENT: prompt is too long");
  CHECK(failure({.status_code = 9, .status_message = "no chat template"}) ==
        "llamad FAILED_PRECONDITION: no chat template");
  CHECK(failure({.text = {"partial"},
                 .status_code = 13,
                 .status_message = "decode failed"}) ==
        "llamad INTERNAL: decode failed");
  // The server cancels without a local cancel: still an error.
  CHECK(failure({.status_code = 1, .status_message = "server shutdown"}) ==
        "llamad CANCELLED: server shutdown");
  CHECK(failure({.finish = std::nullopt}) ==
        "llamad ended the stream without a finish chunk");
  CHECK(failure({.finish = FakeFinishReason::unspecified})
            .starts_with("llamad sent an unknown finish reason"));

  // Nothing scripted: the fake answers INTERNAL.
  run.start();
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().message == "llamad INTERNAL: no scripted reply");
}

TEST_CASE("cancel ends an in-flight request with a cancelled error") {
  Harness run;
  CHECK_FALSE(run.client->cancel());
  run.fake->enqueue({.text = {"thinking"}, .hang = true});
  run.start();
  REQUIRE(run.fake->wait_for_requests(1));
  CHECK_FALSE(run.client->start(Harness::basic_request(), {}));
  REQUIRE(run.client->cancel());
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().cancelled);
  CHECK_FALSE(run.client->cancel());
  CHECK(run.fake->wait_until_idle());

  // The client is usable again after a cancellation.
  run.fake->enqueue(FakeReply::say("again"));
  run.start();
  run.finish();
  CHECK(run.finished->has_value());
}

TEST_CASE("on_finished can start the next request") {
  Harness run;
  run.fake->enqueue(FakeReply::say("one"));
  run.fake->enqueue(FakeReply::say("two"));
  std::vector<std::string> texts;
  std::size_t finished{};
  const auto second = [&] {
    return run.client->start(
        Harness::basic_request(),
        {.on_text = [&](auto text) { texts.emplace_back(text); },
         .on_finished = [&](auto) { ++finished; }});
  };
  REQUIRE(run.client->start(
      Harness::basic_request(),
      {.on_text = [&](auto text) { texts.emplace_back(text); },
       .on_finished =
           [&](auto) {
             ++finished;
             REQUIRE(second());
           }}));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (finished < 2 && std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(run.client->pump());
    std::this_thread::sleep_for(1ms);
  }
  CHECK(finished == 2);
  CHECK(texts == std::vector<std::string>{"one", "two"});
}

TEST_CASE("pump delivers a bounded number of callbacks, and an undelivered "
          "terminal event blocks the next start") {
  Harness run;
  run.fake->enqueue({.text = {"a", "b", "c"}});
  run.start();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  pigpen::llamad::DeliveryStats stats;
  while (stats.events_remaining < 4 &&
         std::chrono::steady_clock::now() < deadline) {
    stats = run.client->pump(0);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(stats.events_remaining == 4);
  // The stream has ended on the worker, but on_finished is not delivered
  // yet, so the request is still active.
  CHECK_FALSE(run.client->start(Harness::basic_request(), {}));
  stats = run.client->pump(1);
  CHECK(stats.callbacks_delivered == 1);
  CHECK(stats.events_remaining == 3);
  CHECK(run.events == std::vector<std::string>{"a"});
  stats = run.client->pump();
  CHECK(stats.callbacks_delivered == 3);
  CHECK(stats.events_remaining == 0);
  CHECK(run.finished);
  run.fake->enqueue(FakeReply::say("next"));
  run.start();
  run.finish();
  CHECK(run.finished->has_value());
}

TEST_CASE("connect rejects a missing daemon and models that cannot chat") {
  pigpen::testing::TempSocketDirectory directory;
  const auto missing = LlamadChatClient::connect(directory.socket());
  REQUIRE_FALSE(missing);
  CHECK(missing.error().starts_with("could not reach llamad at " +
                                    directory.socket()));
  CHECK(missing.error().find("llamad --model") != std::string::npos);
  CHECK_FALSE(LlamadChatClient::connect(""));

  {
    auto fake =
        FakeLlamad::start(directory.socket(), {.description = "bge-small",
                                               .serves_embeddings = true});
    REQUIRE(fake);
    const auto embedding = LlamadChatClient::connect(directory.socket());
    REQUIRE_FALSE(embedding);
    CHECK(embedding.error().find("embedding model (bge-small)") !=
          std::string::npos);
  }
  pigpen::testing::TempSocketDirectory other;
  auto fake = FakeLlamad::start(other.socket(), {.description = "base model",
                                                 .has_chat_template = false});
  REQUIRE(fake);
  const auto no_template = LlamadChatClient::connect(other.socket());
  REQUIRE_FALSE(no_template);
  CHECK(no_template.error().find("has no chat template") != std::string::npos);
}

TEST_CASE("destroying the client with a stream in flight suppresses "
          "delivery") {
  Harness run;
  run.fake->enqueue({.text = {"never delivered"}, .hang = true});
  std::size_t callbacks{};
  REQUIRE(run.client->start(Harness::basic_request(),
                            {.on_text = [&](auto) { ++callbacks; },
                             .on_finished = [&](auto) { ++callbacks; }}));
  REQUIRE(run.fake->wait_for_requests(1));
  run.client.reset();
  CHECK(callbacks == 0);
  // The destructor cancelled the call, so the fake is idle again.
  CHECK(run.fake->wait_until_idle());
}
