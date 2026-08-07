#include "episode_runner.hpp"

#include "agent/prompt.hpp"

#include <algorithm>
#include <utility>

namespace pigpen::agent {

struct EpisodeRunner::SharedState {
  RunState run_state{RunState::idle};
  std::uint32_t turns_used{};
  std::uint32_t turn_budget{};
  bool turn_in_flight{false};
  bool stop_requested{false};
  std::optional<FinishReason> finish_reason{};
  std::string error{};
  std::chrono::steady_clock::time_point turn_started{};
  std::chrono::milliseconds last_turn_latency{};
  std::uint64_t generation{};
  std::optional<TurnOutcome> pending_outcome{};
  std::string active_user_message{};
  std::string queued_user_input{};
  std::size_t active_assistant_entry{};
  std::vector<TranscriptEntry> transcript{};
};

EpisodeRunner::EpisodeRunner(ITurnTransport &transport,
                             const std::uint32_t turn_budget,
                             std::function<bool()> objective_complete,
                             EpisodeObservers observers)
    : transport_(transport), objective_complete_(std::move(objective_complete)),
      observers_(std::move(observers)),
      state_(std::make_shared<SharedState>()) {
  state_->turn_budget = std::max<std::uint32_t>(turn_budget, 1U);
}

EpisodeRunner::~EpisodeRunner() {
  ++state_->generation;
  if (state_->turn_in_flight) {
    static_cast<void>(transport_.cancel());
  }
}

bool EpisodeRunner::play() {
  if (state_->run_state == RunState::finished ||
      state_->run_state == RunState::playing) {
    return false;
  }
  state_->run_state = RunState::playing;
  return true;
}

bool EpisodeRunner::pause() {
  if (state_->run_state != RunState::playing || state_->stop_requested) {
    return false;
  }
  state_->run_state = RunState::paused;
  return true;
}

bool EpisodeRunner::stop() {
  if (state_->run_state == RunState::finished || state_->stop_requested) {
    return false;
  }
  state_->stop_requested = true;
  if (state_->turn_in_flight) {
    static_cast<void>(transport_.cancel());
  } else {
    finish(FinishReason::stopped);
  }
  return true;
}

bool EpisodeRunner::fail(std::string error) {
  if (state_->run_state == RunState::finished) {
    return false;
  }
  if (state_->turn_in_flight) {
    static_cast<void>(transport_.cancel());
    state_->turn_in_flight = false;
    state_->pending_outcome.reset();
    ++state_->generation;
  }
  state_->transcript.push_back({
      .turn = state_->turns_used,
      .role = TranscriptRole::error,
      .text = error,
  });
  finish(FinishReason::error, std::move(error));
  return true;
}

void EpisodeRunner::tick() {
  process_pending_outcome();
  if (state_->run_state != RunState::playing || state_->turn_in_flight) {
    return;
  }
  if (state_->stop_requested) {
    finish(FinishReason::stopped);
    return;
  }
  if (objective_complete_ && objective_complete_()) {
    finish(FinishReason::objective_complete);
    return;
  }
  if (state_->turns_used >= state_->turn_budget) {
    finish(FinishReason::turn_budget);
    return;
  }
  start_turn();
}

void EpisodeRunner::queue_user_input(std::string message) {
  state_->queued_user_input = std::move(message);
}

EpisodeSnapshot EpisodeRunner::snapshot() const {
  return {
      .state = state_->run_state,
      .turns_used = state_->turns_used,
      .turn_budget = state_->turn_budget,
      .turn_in_flight = state_->turn_in_flight,
      .stop_requested = state_->stop_requested,
      .finish_reason = state_->finish_reason,
      .error = state_->error,
      .last_turn_latency = state_->last_turn_latency,
  };
}

const std::vector<TranscriptEntry> &EpisodeRunner::transcript() const noexcept {
  return state_->transcript;
}

void EpisodeRunner::start_turn() {
  const auto turn = state_->turns_used + 1U;
  auto message =
      build_turn_prompt(turn, state_->turn_budget, state_->queued_user_input);
  state_->queued_user_input.clear();

  state_->active_user_message = message;
  state_->transcript.push_back({
      .turn = turn,
      .role = TranscriptRole::user,
      .text = message,
  });
  state_->transcript.push_back({
      .turn = turn,
      .role = TranscriptRole::assistant,
      .text = {},
  });
  state_->active_assistant_entry = state_->transcript.size() - 1U;
  state_->turn_started = std::chrono::steady_clock::now();
  state_->turn_in_flight = true;
  const auto generation = ++state_->generation;
  const std::weak_ptr<SharedState> weak_state{state_};

  auto sent = transport_.send(
      message,
      TurnCallbacks{
          .on_text_delta =
              [weak_state, generation](const std::string_view delta) {
                const auto state = weak_state.lock();
                if (!state || state->generation != generation ||
                    state->active_assistant_entry >= state->transcript.size()) {
                  return;
                }
                state->transcript[state->active_assistant_entry].text.append(
                    delta);
              },
          .on_finished =
              [weak_state, generation](TurnOutcome outcome) {
                const auto state = weak_state.lock();
                if (!state || state->generation != generation ||
                    !state->turn_in_flight) {
                  return;
                }
                state->pending_outcome = std::move(outcome);
              },
      });

  if (!sent) {
    state_->turn_in_flight = false;
    state_->error = std::move(sent.error());
    state_->transcript.push_back({
        .turn = turn,
        .role = TranscriptRole::error,
        .text = state_->error,
    });
    finish(FinishReason::error, state_->error);
  }
}

void EpisodeRunner::process_pending_outcome() {
  if (!state_->pending_outcome) {
    return;
  }
  auto outcome = std::move(*state_->pending_outcome);
  state_->pending_outcome.reset();
  state_->turn_in_flight = false;
  ++state_->turns_used;
  state_->last_turn_latency =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - state_->turn_started);

  auto &assistant = state_->transcript[state_->active_assistant_entry].text;
  if (assistant.empty()) {
    assistant = outcome.text;
  }
  const TurnRecord record{
      .turn = state_->turns_used,
      .status = outcome.status,
      .user_message = state_->active_user_message,
      .assistant_text = assistant,
      .error = outcome.error,
      .input_tokens = outcome.input_tokens,
      .output_tokens = outcome.output_tokens,
      .latency = state_->last_turn_latency,
  };
  if (observers_.on_turn_finished) {
    observers_.on_turn_finished(record);
  }

  if (outcome.status == TurnStatus::error) {
    state_->transcript.push_back({
        .turn = state_->turns_used,
        .role = TranscriptRole::error,
        .text = outcome.error,
    });
    finish(FinishReason::error, std::move(outcome.error));
    return;
  }
  if (outcome.status == TurnStatus::cancelled) {
    if (state_->stop_requested) {
      finish(FinishReason::stopped);
    } else {
      finish(FinishReason::cancelled, std::move(outcome.error));
    }
    return;
  }
  if (state_->stop_requested) {
    finish(FinishReason::stopped);
    return;
  }
  if (objective_complete_ && objective_complete_()) {
    finish(FinishReason::objective_complete);
    return;
  }
  if (state_->turns_used >= state_->turn_budget) {
    finish(FinishReason::turn_budget);
  }
}

void EpisodeRunner::finish(const FinishReason reason, std::string error) {
  if (state_->run_state == RunState::finished) {
    return;
  }
  state_->run_state = RunState::finished;
  state_->finish_reason = reason;
  state_->error = std::move(error);
  if (observers_.on_episode_finished) {
    observers_.on_episode_finished({
        .reason = reason,
        .turns_used = state_->turns_used,
        .error = state_->error,
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

} // namespace pigpen::agent
