#include "agent/episode_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public pigpen::agent::ITurnTransport {
public:
  std::expected<void, std::string>
  send(std::string user_message,
       pigpen::agent::TurnCallbacks callbacks) override {
    if (send_error) {
      return std::unexpected(*send_error);
    }
    ++send_count;
    messages.push_back(std::move(user_message));
    callbacks_ = std::move(callbacks);
    active = true;
    return {};
  }

  bool cancel() noexcept override {
    ++cancel_count;
    return active;
  }

  void delta(const std::string &text) {
    REQUIRE(active);
    callbacks_.on_text_delta(text);
  }

  void complete(pigpen::agent::TurnOutcome outcome = {}) {
    REQUIRE(active);
    active = false;
    callbacks_.on_finished(std::move(outcome));
  }

  bool active{false};
  int send_count{};
  int cancel_count{};
  std::optional<std::string> send_error{};
  std::vector<std::string> messages{};

private:
  pigpen::agent::TurnCallbacks callbacks_{};
};

} // namespace

TEST_CASE("episode runner automatically advances until its turn budget") {
  FakeTransport transport;
  std::vector<pigpen::agent::TurnRecord> turns;
  std::optional<pigpen::agent::EpisodeResult> result;
  pigpen::agent::EpisodeRunner runner{
      transport,
      2,
      [] { return false; },
      {
          .on_turn_finished =
              [&turns](const auto &turn) { turns.push_back(turn); },
          .on_episode_finished =
              [&result](const auto &episode) { result = episode; },
      }};

  REQUIRE(runner.play());
  runner.tick();
  REQUIRE(transport.send_count == 1);
  REQUIRE(transport.messages[0].find("Turn 1 of 2") != std::string::npos);
  transport.delta("I will explore. ");
  transport.complete({.text = "done"});
  runner.tick();
  REQUIRE(transport.send_count == 2);
  transport.complete({.text = "second"});
  runner.tick();

  const auto snapshot = runner.snapshot();
  REQUIRE(snapshot.state == pigpen::agent::RunState::finished);
  REQUIRE(snapshot.turns_used == 2);
  REQUIRE(snapshot.finish_reason == pigpen::agent::FinishReason::turn_budget);
  REQUIRE(result.has_value());
  REQUIRE(turns.size() == 2);
  REQUIRE(turns[0].assistant_text == "I will explore. ");
}

TEST_CASE("pause takes effect between turns and resume continues") {
  FakeTransport transport;
  pigpen::agent::EpisodeRunner runner{transport, 3, [] { return false; }};

  REQUIRE(runner.play());
  runner.tick();
  REQUIRE(runner.pause());
  REQUIRE(runner.snapshot().state == pigpen::agent::RunState::paused);
  transport.complete();
  runner.tick();
  REQUIRE(transport.send_count == 1);
  REQUIRE(runner.snapshot().turns_used == 1);

  REQUIRE(runner.play());
  runner.tick();
  REQUIRE(transport.send_count == 2);
}

TEST_CASE("stop cancels an in-flight turn and finishes after cancellation") {
  FakeTransport transport;
  pigpen::agent::EpisodeRunner runner{transport, 3, [] { return false; }};

  REQUIRE(runner.play());
  runner.tick();
  REQUIRE(runner.stop());
  REQUIRE(transport.cancel_count == 1);
  REQUIRE(runner.snapshot().stop_requested);
  transport.complete({
      .status = pigpen::agent::TurnStatus::cancelled,
      .error = "cancelled",
  });
  runner.tick();

  REQUIRE(runner.snapshot().state == pigpen::agent::RunState::finished);
  REQUIRE(runner.snapshot().finish_reason ==
          pigpen::agent::FinishReason::stopped);
}

TEST_CASE("objective completion ends without sending another turn") {
  FakeTransport transport;
  bool complete = false;
  pigpen::agent::EpisodeRunner runner{transport, 4,
                                      [&complete] { return complete; }};

  REQUIRE(runner.play());
  runner.tick();
  complete = true;
  transport.complete();
  runner.tick();

  REQUIRE(transport.send_count == 1);
  REQUIRE(runner.snapshot().finish_reason ==
          pigpen::agent::FinishReason::objective_complete);
}

TEST_CASE("terminal turn errors are surfaced without retry") {
  FakeTransport transport;
  pigpen::agent::EpisodeRunner runner{transport, 4, [] { return false; }};

  REQUIRE(runner.play());
  runner.tick();
  transport.complete({
      .status = pigpen::agent::TurnStatus::error,
      .error = "provider failed",
  });
  runner.tick();

  REQUIRE(transport.send_count == 1);
  REQUIRE(runner.snapshot().finish_reason ==
          pigpen::agent::FinishReason::error);
  REQUIRE(runner.snapshot().error == "provider failed");
  REQUIRE(runner.transcript().back().role ==
          pigpen::agent::TranscriptRole::error);
}

TEST_CASE(
    "host failures cancel active work and become terminal episode errors") {
  FakeTransport transport;
  pigpen::agent::EpisodeRunner runner{transport, 4, [] { return false; }};

  REQUIRE(runner.play());
  runner.tick();
  REQUIRE(runner.fail("metrics sink failed"));

  CHECK(transport.cancel_count == 1);
  CHECK(runner.snapshot().state == pigpen::agent::RunState::finished);
  CHECK(runner.snapshot().finish_reason == pigpen::agent::FinishReason::error);
  CHECK(runner.snapshot().error == "metrics sink failed");
  CHECK(runner.transcript().back().role ==
        pigpen::agent::TranscriptRole::error);
}

TEST_CASE("immediate transport admission failure terminates the episode") {
  FakeTransport transport;
  transport.send_error = "busy";
  pigpen::agent::EpisodeRunner runner{transport, 4, [] { return false; }};

  REQUIRE(runner.play());
  runner.tick();

  REQUIRE(runner.snapshot().finish_reason ==
          pigpen::agent::FinishReason::error);
  REQUIRE(runner.snapshot().turns_used == 0);
  REQUIRE(runner.snapshot().error == "busy");
}

TEST_CASE("queued human input is appended to the next automatic nudge") {
  FakeTransport transport;
  pigpen::agent::EpisodeRunner runner{transport, 1, [] { return false; }};
  runner.queue_user_input("look west first");

  REQUIRE(runner.play());
  runner.tick();

  REQUIRE(transport.messages.size() == 1);
  REQUIRE(transport.messages[0].find("Human input: look west first") !=
          std::string::npos);
}
