/// @file llamad_client_tests.cpp
/// @brief Covers LlamadChatClient against the scripted fake llamad over a
/// real gRPC Unix socket: the wire mapping both ways, stream order, failure
/// statuses, a daemon that stops mid-stream, cancellation (also in a race
/// with the worker), the stall watchdog, the connect deadline, connect
/// rejections, and delivery after destruction.

#include "llamad/llamad_client.hpp"

#include "support/fake_llamad.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using pigpen::core::ChatError;
using pigpen::core::ChatErrorKind;
using pigpen::core::ChatFinish;
using pigpen::core::ChatFinishReason;
using pigpen::core::ChatRole;
using pigpen::llamad::LlamadChatClient;
using pigpen::testing::FakeFinishReason;
using pigpen::testing::FakeLlamad;
using pigpen::testing::FakeReply;
using pigpen::testing::ReceivedRequest;

namespace {

using namespace std::chrono_literals;

using Finished = std::expected<ChatFinish, ChatError>;

/// @brief Pump @p client until @p done is true or 5 s pass.
[[nodiscard]] bool pump_until(LlamadChatClient &client,
                              const std::function<bool()> &done) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(client.pump());
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

/// @brief Holds the fake's Chat handlers in on_request until open(), so the
/// fake cannot reply before the test acts. A wait ends after 5 s, so a
/// failed test cannot block the fake's shutdown forever.
class Gate final {
public:
  void open() {
    {
      const std::scoped_lock lock{mutex_};
      open_ = true;
    }
    changed_.notify_all();
  }

  void wait() {
    std::unique_lock lock{mutex_};
    static_cast<void>(changed_.wait_for(lock, 5s, [this] { return open_; }));
  }

private:
  std::mutex mutex_{};
  std::condition_variable changed_{};
  bool open_{};
};

struct Harness {
  explicit Harness(pigpen::testing::FakeModelInfo info = {},
                   std::function<void(const ReceivedRequest &)> on_request = {},
                   pigpen::llamad::ConnectOptions options = {})
      : fake(FakeLlamad::start(directory.socket(), std::move(info),
                               std::move(on_request))) {
    REQUIRE(fake);
    auto connected = LlamadChatClient::connect(directory.socket(), options);
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
               .n_ctx_train = 32768,
               .model_name = "Qwen2 0.5B Instruct",
               .model_file = "qwen2-0_5b-instruct-q4_k_m.gguf",
               .llamad_version = "0.2.1",
               .max_request_bytes = 1'048'576}};
  CHECK(run.client->model_info() ==
        pigpen::core::ModelInfo{.description = "qwen2 0.5B Q4_K - Medium",
                                .name = "Qwen2 0.5B Instruct",
                                .file = "qwen2-0_5b-instruct-q4_k_m.gguf",
                                .n_params = 494'000'000,
                                .size_bytes = 400'000'000,
                                .n_ctx = 8192,
                                .n_ctx_train = 32768,
                                .daemon_version = "0.2.1",
                                .max_request_bytes = 1'048'576});
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
    CHECK(run.finished->error().kind == ChatErrorKind::failure);
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
  CHECK(run.finished->error().kind == ChatErrorKind::failure);
  CHECK(run.finished->error().message == "llamad INTERNAL: no scripted reply");
}

TEST_CASE("OUT_OF_RANGE is a prompt that is too long, with the daemon's "
          "message") {
  Harness run;
  run.fake->enqueue({.status_code = 11,
                     .status_message =
                         "prompt is too long: 4140 tokens for a context of "
                         "4096"});
  run.start();
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().kind == ChatErrorKind::prompt_too_long);
  CHECK(run.finished->error().message ==
        "llamad OUT_OF_RANGE: prompt is too long: 4140 tokens for a context "
        "of 4096");

  // Another llamad build answered INVALID_ARGUMENT with this message. Only
  // OUT_OF_RANGE, the status of llamad 0.2.0, is classified.
  run.fake->enqueue(
      {.status_code = 3, .status_message = "prompt is too long: 4140 tokens"});
  run.start();
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().kind == ChatErrorKind::failure);
}

TEST_CASE("a daemon that stops mid-stream ends the request with an error, "
          "not a cancellation") {
  Harness run;
  run.fake->enqueue({.text = {"partial"}, .hang = true});
  run.start();
  REQUIRE(run.fake->wait_for_requests(1));
  // The server shuts down and drops the call that hangs. The status name
  // and message depend on the gRPC version, so the test does not pin them.
  run.fake.reset();
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().kind == ChatErrorKind::failure);
  CHECK(run.finished->error().message.starts_with("llamad "));
  CHECK(std::ranges::count(run.events, std::string{"<finished>"}) == 1);
  CHECK_FALSE(run.client->cancel());
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
  CHECK(run.finished->error().kind == ChatErrorKind::cancelled);
  CHECK_FALSE(run.client->cancel());
  CHECK(run.fake->wait_until_idle());

  // The client is usable again after a cancellation.
  run.fake->enqueue(FakeReply::say("again"));
  run.start();
  run.finish();
  CHECK(run.finished->has_value());
}

TEST_CASE("cancel immediately after start ends the request as cancelled") {
  // The gate holds a request that reaches the fake until after cancel(), so
  // the fake cannot finish it first. Both replies are the same: the
  // follow-up gets one whether or not the first request reached the fake.
  Gate gate;
  Harness run{{}, [&gate](const ReceivedRequest &) { gate.wait(); }};
  run.fake->enqueue(FakeReply::say("ok"));
  run.fake->enqueue(FakeReply::say("ok"));
  std::size_t first_finished{};
  std::optional<Finished> first;
  REQUIRE(run.client->start(Harness::basic_request(),
                            {.on_finished = [&](Finished result) {
                              ++first_finished;
                              first = std::move(result);
                            }}));
  // No wait for the daemon: the worker may not have started the call yet.
  REQUIRE(run.client->cancel());
  gate.open();
  REQUIRE(pump_until(*run.client, [&] { return first.has_value(); }));
  REQUIRE_FALSE(first->has_value());
  CHECK(first->error().kind == ChatErrorKind::cancelled);
  CHECK_FALSE(run.client->cancel());

  // The client is usable again. start() joins the first worker, so every
  // event of the first request is queued before the follow-up starts, and
  // the follow-up's pump drains them.
  run.start();
  run.finish();
  REQUIRE(run.finished->has_value());
  CHECK(run.events == std::vector<std::string>{"ok", "<finished>"});
  CHECK(first_finished == 1);
}

TEST_CASE("cancel after the stream ended but before delivery keeps the "
          "result that the daemon sent") {
  Harness run;
  run.fake->enqueue({.text = {"a"}, .finish = FakeFinishReason::stop});
  std::size_t finished{};
  std::optional<Finished> result;
  std::vector<std::string> texts;
  REQUIRE(run.client->start(
      Harness::basic_request(),
      {.on_text = [&](auto text) { texts.emplace_back(text); },
       .on_finished =
           [&](Finished value) {
             ++finished;
             result = std::move(value);
           }}));
  // pump(0) delivers nothing. Two queued events (the text, then the
  // terminal event, queued after Finish() returned) show that the stream
  // ended on the worker.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  pigpen::llamad::DeliveryStats stats;
  while (stats.events_remaining < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    stats = run.client->pump(0);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(stats.events_remaining == 2);

  // The request stays active until on_finished, so cancel() accepts it. But
  // the daemon already finished the generation, and the result reports what
  // actually happened: the daemon's finish, not a cancellation.
  CHECK(run.client->cancel());
  REQUIRE(pump_until(*run.client, [&] { return result.has_value(); }));
  REQUIRE(result->has_value());
  CHECK((*result)->reason == ChatFinishReason::stop);
  CHECK(texts == std::vector<std::string>{"a"});
  CHECK_FALSE(run.client->cancel());

  // The late cancel does not leak into the next request: a CANCELLED status
  // from the server is again an error that is not marked cancelled.
  run.fake->enqueue({.status_code = 1, .status_message = "server shutdown"});
  run.start();
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().kind == ChatErrorKind::failure);
  CHECK(run.finished->error().message == "llamad CANCELLED: server shutdown");
  CHECK(finished == 1);
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

TEST_CASE("requests ask for no reasoning unless they say otherwise") {
  Harness run;
  run.fake->enqueue(FakeReply::say("one"));
  run.fake->enqueue(FakeReply::say("two"));
  run.start();
  run.finish();
  auto request = Harness::basic_request();
  request.reasoning = pigpen::core::ChatReasoning::model_default;
  run.start(std::move(request));
  run.finish();
  const auto requests = run.fake->requests();
  REQUIRE(requests.size() == 2);
  CHECK(requests[0].reasoning == "REASONING_OFF");
  CHECK(requests[1].reasoning == "REASONING_DEFAULT");
}

TEST_CASE("progress chunks are skipped, and the finish carries the "
          "timings") {
  Harness run;
  run.fake->enqueue({.text = {"I ", "look."},
                     .progress = 2,
                     .prompt_tokens = 64,
                     .completion_tokens = 5,
                     .prompt_ms = 812.5,
                     .completion_ms = 1'204.25});
  run.start();
  run.finish();
  // Two progress chunks before each text chunk and before the finish: only
  // the text and the finish arrive.
  CHECK(run.events == std::vector<std::string>{"I ", "look.", "<finished>"});
  REQUIRE(run.finished->has_value());
  const auto &finish = **run.finished;
  CHECK(finish.reason == ChatFinishReason::end_of_generation);
  CHECK(finish.stats.prompt_tokens == 64);
  CHECK(finish.stats.completion_tokens == 5);
  CHECK(finish.stats.prompt_ms == 812.5);
  CHECK(finish.stats.completion_ms == 1'204.25);

  // A request that is cancelled after progress chunks still ends cancelled,
  // without a finish chunk.
  run.fake->enqueue({.text = {"slow"}, .progress = 1, .hang = true});
  run.start();
  REQUIRE(run.fake->wait_for_requests(2));
  REQUIRE(run.client->cancel());
  run.finish();
  REQUIRE_FALSE(run.finished->has_value());
  CHECK(run.finished->error().kind == ChatErrorKind::cancelled);
}

TEST_CASE("connect refuses a daemon outside the llamad 0.2 series") {
  pigpen::testing::TempSocketDirectory directory;
  {
    // A 0.1.x daemon has no version field, so it reports it empty.
    auto fake = FakeLlamad::start(directory.socket(), {.llamad_version = ""});
    REQUIRE(fake);
    const auto older = LlamadChatClient::connect(directory.socket());
    REQUIRE_FALSE(older);
    CHECK(older.error() == "llamad at " + directory.socket() +
                               " is older than 0.2.0, but this Pig Pen needs "
                               "llamad 0.2.x");
  }
  pigpen::testing::TempSocketDirectory other;
  auto fake = FakeLlamad::start(other.socket(), {.llamad_version = "0.3.0"});
  REQUIRE(fake);
  const auto newer = LlamadChatClient::connect(other.socket());
  REQUIRE_FALSE(newer);
  CHECK(newer.error() == "llamad at " + other.socket() +
                             " is version 0.3.0, but this Pig Pen needs "
                             "llamad 0.2.x");
  CHECK(fake->requests().empty());
}

namespace {

/// @brief A harness whose requests stop after @p stall_timeout of silence.
[[nodiscard]] std::unique_ptr<Harness>
stall_harness(const std::chrono::milliseconds stall_timeout) {
  return std::make_unique<Harness>(
      pigpen::testing::FakeModelInfo{}, nullptr,
      pigpen::llamad::ConnectOptions{.stall_timeout = stall_timeout});
}

/// @brief Pump @p client for @p duration; false when @p finished turns true.
[[nodiscard]] bool
pump_while_unfinished(LlamadChatClient &client,
                      const std::chrono::milliseconds duration,
                      const std::size_t &finished) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
    static_cast<void>(client.pump());
    if (finished != 0) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return finished == 0;
}

} // namespace

TEST_CASE("a silent stream ends as a stall failure, exactly once, and the "
          "client is usable afterwards") {
  constexpr auto stall_timeout = 200ms;
  auto run = stall_harness(stall_timeout);
  run->fake->enqueue({.text = {"thinking"}, .hang = true});
  std::size_t finished{};
  std::optional<Finished> result;
  std::vector<std::string> texts;
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(run->client->start(
      Harness::basic_request(),
      {.on_text = [&](auto text) { texts.emplace_back(text); },
       .on_finished =
           [&](Finished value) {
             ++finished;
             result = std::move(value);
           }}));
  REQUIRE(pump_until(*run->client, [&] { return result.has_value(); }));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  // The watchdog counts the silence after the last chunk, so it cannot fire
  // before the stall timeout.
  CHECK(elapsed >= stall_timeout);
  REQUIRE_FALSE(result->has_value());
  CHECK(result->error().kind == ChatErrorKind::failure);
  CHECK(result->error().message ==
        "llamad sent nothing for 0.2 seconds, so Pig Pen stopped the request");
  CHECK(texts == std::vector<std::string>{"thinking"});
  CHECK_FALSE(run->client->cancel());
  // The watchdog cancelled the call, so the fake is idle again.
  CHECK(run->fake->wait_until_idle());

  // The client is usable again, and the stalled request delivers nothing
  // more.
  run->fake->enqueue(FakeReply::say("again"));
  run->start();
  run->finish();
  REQUIRE(run->finished->has_value());
  CHECK(run->events == std::vector<std::string>{"again", "<finished>"});
  CHECK(finished == 1);
}

TEST_CASE("progress chunks keep a slow request alive past the stall "
          "timeout, and a local cancel then ends it as cancelled") {
  // A live daemon that queues the request sends a progress chunk now and
  // then. The interval is far below the stall timeout, and the request is
  // watched for five stall timeouts.
  constexpr auto stall_timeout = 150ms;
  auto run = stall_harness(stall_timeout);
  run->fake->enqueue(
      {.text = {"queued"}, .hang = true, .hang_progress_interval = 25ms});
  std::size_t finished{};
  std::optional<Finished> result;
  REQUIRE(run->client->start(Harness::basic_request(),
                             {.on_finished = [&](Finished value) {
                               ++finished;
                               result = std::move(value);
                             }}));
  REQUIRE(run->fake->wait_for_requests(1));
  CHECK(pump_while_unfinished(*run->client, 5 * stall_timeout, finished));

  REQUIRE(run->client->cancel());
  REQUIRE(pump_until(*run->client, [&] { return result.has_value(); }));
  REQUIRE_FALSE(result->has_value());
  CHECK(result->error().kind == ChatErrorKind::cancelled);
  CHECK(finished == 1);
  CHECK(run->fake->wait_until_idle());
}

TEST_CASE("a stall timeout of zero never stops a silent stream") {
  auto run = stall_harness(0ms);
  run->fake->enqueue({.hang = true});
  std::size_t finished{};
  std::optional<Finished> result;
  REQUIRE(run->client->start(Harness::basic_request(),
                             {.on_finished = [&](Finished value) {
                               ++finished;
                               result = std::move(value);
                             }}));
  REQUIRE(run->fake->wait_for_requests(1));
  CHECK(pump_while_unfinished(*run->client, 300ms, finished));
  REQUIRE(run->client->cancel());
  REQUIRE(pump_until(*run->client, [&] { return result.has_value(); }));
  REQUIRE_FALSE(result->has_value());
  CHECK(result->error().kind == ChatErrorKind::cancelled);
  CHECK(finished == 1);
}

TEST_CASE("connect gives up at its deadline when GetModelInfo does not "
          "answer") {
  pigpen::testing::TempSocketDirectory directory;
  // Written on a server thread.
  std::atomic<std::size_t> calls{};
  auto fake = FakeLlamad::start(directory.socket(), {.reply_delay = 30s}, {},
                                [&calls] { ++calls; });
  REQUIRE(fake);
  const auto started = std::chrono::steady_clock::now();
  const auto connected =
      LlamadChatClient::connect(directory.socket(), {.deadline = 200ms});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  REQUIRE_FALSE(connected);
  CHECK(connected.error().starts_with(
      "could not reach llamad at " + directory.socket() +
      ": it did not answer within 0.2 seconds (llamad DEADLINE_EXCEEDED"));
  // The call reached the fake, and connect waited for the deadline, but not
  // for the delay of the fake or for the default deadline of 5 s.
  CHECK(calls == 1);
  CHECK(elapsed >= 150ms);
  CHECK(elapsed < 3s);
}

TEST_CASE("connect reports a GetModelInfo failure status") {
  pigpen::testing::TempSocketDirectory directory;
  auto fake = FakeLlamad::start(
      directory.socket(),
      {.status_code = 13, .status_message = "the model is loading"});
  REQUIRE(fake);
  const auto connected = LlamadChatClient::connect(directory.socket());
  REQUIRE_FALSE(connected);
  CHECK(connected.error() == "llamad at " + directory.socket() +
                                 " did not report its model (llamad INTERNAL: "
                                 "the model is loading)");
}
