/// @file episode_runner.cpp
/// @brief EpisodeRunner implementation; the contract is in the header.
#include "core/episode_runner.hpp"

#include "core/prompt.hpp"

#include <algorithm>
#include <utility>

namespace pigpen::core {

EpisodeRunner::EpisodeRunner(ITurnTransport &transport,
                             const std::uint32_t turn_budget,
                             std::function<bool()> objective_complete,
                             EpisodeObservers observers,
                             std::function<std::size_t()> tool_call_count)
    : transport_(transport), objective_complete_(std::move(objective_complete)),
      tool_call_count_(std::move(tool_call_count)),
      observers_(std::move(observers)), snapshot_{.turn_budget = turn_budget} {}

EpisodeRunner::~EpisodeRunner() {
  if (snapshot_.turn_in_flight) {
    transport_.cancel();
  }
}

bool EpisodeRunner::play() {
  if (snapshot_.state == RunState::finished ||
      snapshot_.state == RunState::playing) {
    return false;
  }
  snapshot_.state = RunState::playing;
  return true;
}

bool EpisodeRunner::pause() {
  if (snapshot_.state != RunState::playing || stop_requested_) {
    return false;
  }
  snapshot_.state = RunState::paused;
  return true;
}

bool EpisodeRunner::stop() {
  if (snapshot_.state == RunState::finished || stop_requested_) {
    return false;
  }
  stop_requested_ = true;
  if (snapshot_.turn_in_flight) {
    transport_.cancel();
  } else {
    finish(FinishReason::stopped);
  }
  return true;
}

bool EpisodeRunner::fail(std::string error) {
  if (snapshot_.state == RunState::finished) {
    return false;
  }
  if (snapshot_.turn_in_flight) {
    transport_.cancel();
    snapshot_.turn_in_flight = false;
    pending_outcome_.reset();
  }
  transcript_.push_back({
      .turn = snapshot_.turns_used,
      .role = TranscriptRole::error,
      .text = error,
  });
  finish(FinishReason::error, std::move(error));
  return true;
}

void EpisodeRunner::tick() {
  process_pending_outcome();
  if (snapshot_.state == RunState::playing && !snapshot_.turn_in_flight &&
      !finish_if_due()) {
    start_turn();
  }
}

std::uint64_t EpisodeRunner::queue_user_input(std::string message) {
  const auto id = next_guidance_id_++;
  guidance_.push_back({.id = id, .text = std::move(message)});
  update_pending_guidance_turns();
  return id;
}

bool EpisodeRunner::remove_pending_user_input(const std::uint64_t id) {
  const auto removed = std::erase_if(guidance_, [id](const auto &guidance) {
    return guidance.id == id && guidance.status == GuidanceStatus::pending;
  });
  update_pending_guidance_turns();
  return removed != 0;
}

void EpisodeRunner::clear_pending_user_inputs() {
  std::erase_if(guidance_, [](const auto &guidance) {
    return guidance.status == GuidanceStatus::pending;
  });
}

void EpisodeRunner::start_turn() {
  const auto turn = snapshot_.turns_used + 1U;
  const auto guidance = std::ranges::find(guidance_, GuidanceStatus::pending,
                                          &GuidanceEntry::status);
  const auto has_guidance = guidance != guidance_.end();
  const auto prompt = [&](const std::string_view human_input) {
    return build_turn_prompt(turn, snapshot_.turn_budget, human_input,
                             recover_zero_tool_turn_, unexecuted_tool_calls_);
  };

  active_user_message_ = prompt(has_guidance ? guidance->text : "");
  turn_started_ = std::chrono::steady_clock::now();
  tool_calls_at_turn_start_ = tool_call_count_ ? tool_call_count_() : 0U;
  auto sent = transport_.send(active_user_message_,
                              {
                                  .on_text_delta =
                                      [this](const std::string_view delta) {
                                        if (snapshot_.turn_in_flight) {
                                          transcript_.back().text.append(delta);
                                        }
                                      },
                                  .on_finished =
                                      [this](TurnOutcome outcome) {
                                        if (snapshot_.turn_in_flight) {
                                          pending_outcome_ = std::move(outcome);
                                        }
                                      },
                              });
  if (!sent) {
    transcript_.push_back(
        {.turn = turn, .role = TranscriptRole::error, .text = sent.error()});
    finish(FinishReason::error, std::move(sent.error()));
    return;
  }

  // Transport callbacks are asynchronous, so the transcript can be written
  // after send(); the assistant entry stays last while the turn is in flight.
  snapshot_.turn_in_flight = true;
  transcript_.push_back(
      {.turn = turn, .role = TranscriptRole::automatic, .text = prompt({})});
  if (has_guidance) {
    transcript_.push_back({.turn = turn,
                           .role = TranscriptRole::guidance,
                           .text = guidance->text});
    guidance->status = GuidanceStatus::sent;
  }
  transcript_.push_back({.turn = turn, .role = TranscriptRole::assistant});
}

void EpisodeRunner::process_pending_outcome() {
  if (!pending_outcome_) {
    return;
  }
  auto outcome = *std::exchange(pending_outcome_, std::nullopt);
  snapshot_.turn_in_flight = false;
  ++snapshot_.turns_used;
  snapshot_.last_turn_latency =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - turn_started_);
  const auto tool_calls =
      tool_call_count_ ? tool_call_count_() - tool_calls_at_turn_start_ : 0U;
  recover_zero_tool_turn_ = outcome.status == TurnStatus::completed &&
                            tool_call_count_ && tool_calls == 0U;
  unexecuted_tool_calls_ =
      outcome.tool_stats ? outcome.tool_stats->unexecuted_calls : 0U;

  auto &assistant = transcript_.back().text;
  if (assistant.empty()) {
    assistant = outcome.text;
  }
  if (observers_.on_turn_finished) {
    observers_.on_turn_finished({
        .turn = snapshot_.turns_used,
        .status = outcome.status,
        .user_message = active_user_message_,
        .assistant_text = assistant,
        .error = outcome.error,
        .input_tokens = outcome.input_tokens,
        .output_tokens = outcome.output_tokens,
        .tool_calls = tool_calls,
        .latency = snapshot_.last_turn_latency,
        .tool_stats = outcome.tool_stats,
    });
  }
  if (snapshot_.state == RunState::finished) {
    return;
  }

  if (outcome.status == TurnStatus::error) {
    transcript_.push_back({
        .turn = snapshot_.turns_used,
        .role = TranscriptRole::error,
        .text = outcome.error,
    });
    finish(FinishReason::error, std::move(outcome.error));
  } else if (outcome.status == TurnStatus::cancelled && !stop_requested_) {
    finish(FinishReason::cancelled, std::move(outcome.error));
  } else {
    finish_if_due();
  }
}

bool EpisodeRunner::finish_if_due() {
  if (stop_requested_) {
    finish(FinishReason::stopped);
  } else if (objective_complete_()) {
    finish(FinishReason::objective_complete);
  } else if (snapshot_.turns_used >= snapshot_.turn_budget) {
    finish(FinishReason::turn_budget);
  }
  return snapshot_.state == RunState::finished;
}

void EpisodeRunner::update_pending_guidance_turns() {
  auto turn = snapshot_.turns_used + (snapshot_.turn_in_flight ? 2U : 1U);
  for (auto &guidance : guidance_) {
    if (guidance.status == GuidanceStatus::pending) {
      guidance.turn = turn++;
    }
  }
}

void EpisodeRunner::finish(const FinishReason reason, std::string error) {
  if (snapshot_.state == RunState::finished) {
    return;
  }
  snapshot_.state = RunState::finished;
  snapshot_.finish_reason = reason;
  snapshot_.error = std::move(error);
  if (observers_.on_episode_finished) {
    observers_.on_episode_finished({
        .reason = reason,
        .turns_used = snapshot_.turns_used,
        .error = snapshot_.error,
    });
  }
}

std::string_view run_state_name(const RunState state) noexcept {
  switch (state) {
  case RunState::idle:
    return "idle";
  case RunState::playing:
    return "playing";
  case RunState::paused:
    return "paused";
  case RunState::finished:
    return "finished";
  }
  return "unknown";
}

std::string_view finish_reason_name(const FinishReason reason) noexcept {
  switch (reason) {
  case FinishReason::turn_budget:
    return "turn_budget";
  case FinishReason::objective_complete:
    return "objective_complete";
  case FinishReason::stopped:
    return "stopped";
  case FinishReason::cancelled:
    return "cancelled";
  case FinishReason::error:
    return "error";
  }
  return "unknown";
}

} // namespace pigpen::core
