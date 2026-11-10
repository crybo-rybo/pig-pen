/// @file tool_loop.cpp
/// @brief ToolLoopTransport implementation; the contract is in the header.
#include "core/tool_loop.hpp"

#include <utility>

namespace pigpen::core {

ToolLoopTransport::ToolLoopTransport(IChatClient &client,
                                     IToolDispatcher &dispatcher,
                                     std::string system_prompt,
                                     ChatSampling sampling,
                                     const ToolLoopLimits limits)
    : client_(client), dispatcher_(dispatcher), sampling_(sampling),
      limits_(limits), alive_(std::make_shared<ToolLoopTransport *>(this)) {
  history_.push_back(
      {.role = ChatRole::system, .content = std::move(system_prompt)});
}

ToolLoopTransport::~ToolLoopTransport() {
  alive_.reset();
  if (turn_) {
    client_.cancel();
  }
}

std::expected<void, std::string>
ToolLoopTransport::send(std::string user_message, TurnCallbacks callbacks) {
  if (turn_) {
    return std::unexpected("a model turn is already active");
  }
  turn_.emplace(ActiveTurn{.callbacks = std::move(callbacks)});
  turn_->staged.push_back(
      {.role = ChatRole::user, .content = std::move(user_message)});
  if (auto started = start_round(); !started) {
    turn_.reset();
    return started;
  }
  return {};
}

bool ToolLoopTransport::cancel() noexcept {
  if (!turn_ || turn_->cancel_requested) {
    return false;
  }
  turn_->cancel_requested = true;
  static_cast<void>(client_.cancel());
  return true;
}

std::expected<void, std::string> ToolLoopTransport::start_round() {
  ChatRequest request{.tools = dispatcher_.tools(), .sampling = sampling_};
  request.messages.reserve(history_.size() + turn_->staged.size());
  request.messages.insert(request.messages.end(), history_.begin(),
                          history_.end());
  request.messages.insert(request.messages.end(), turn_->staged.begin(),
                          turn_->staged.end());
  turn_->round_text.clear();

  std::weak_ptr<ToolLoopTransport *> alive = alive_;
  auto started = client_.start(
      std::move(request),
      {
          .on_text =
              [alive](const std::string_view text) {
                const auto self = alive.lock();
                if (!self || !(*self)->turn_) {
                  return;
                }
                auto &turn = *(*self)->turn_;
                turn.round_text.append(text);
                turn.outcome.text.append(text);
                if (turn.callbacks.on_text_delta) {
                  turn.callbacks.on_text_delta(text);
                }
              },
          .on_finished =
              [alive](std::expected<ChatFinish, ChatError> finished) {
                if (const auto self = alive.lock(); self && (*self)->turn_) {
                  (*self)->on_round_finished(std::move(finished));
                }
              },
      });
  if (started) {
    ++turn_->outcome.requests;
  }
  return started;
}

void ToolLoopTransport::on_round_finished(
    std::expected<ChatFinish, ChatError> finished) {
  auto &turn = *turn_;
  if (!finished) {
    const auto cancelled = finished.error().cancelled || turn.cancel_requested;
    end_turn(cancelled ? TurnStatus::cancelled : TurnStatus::error,
             std::move(finished.error().message));
    return;
  }

  auto &outcome = turn.outcome;
  outcome.input_tokens += finished->stats.prompt_tokens;
  outcome.output_tokens += finished->stats.completion_tokens;
  outcome.cached_input_tokens += finished->stats.cached_prompt_tokens;

  switch (finished->reason) {
  case ChatFinishReason::end_of_generation:
  case ChatFinishReason::stop:
    if (!turn.round_text.empty()) {
      turn.staged.push_back(
          {.role = ChatRole::assistant, .content = turn.round_text});
    }
    complete_turn();
    return;
  case ChatFinishReason::length:
    end_turn(TurnStatus::error,
             "model response reached its output-token limit");
    return;
  case ChatFinishReason::cancelled:
    end_turn(TurnStatus::cancelled, "the model turn was cancelled");
    return;
  case ChatFinishReason::tool_calls:
    break;
  }

  if (finished->tool_calls.empty()) {
    end_turn(TurnStatus::error,
             "model response reported tool calls without any call");
    return;
  }
  if (turn.cancel_requested) {
    end_turn(TurnStatus::cancelled, "the model turn was cancelled");
    return;
  }
  auto &stats = outcome.tool_stats;
  if (stats.rounds >= limits_.max_tool_rounds) {
    // Round-limit completion: the calls never run, and the history keeps
    // only the reply's text, so it never holds an unanswered call.
    if (!turn.round_text.empty()) {
      turn.staged.push_back(
          {.role = ChatRole::assistant, .content = turn.round_text});
    }
    stats.round_limit_reached = true;
    stats.unexecuted_calls = finished->tool_calls.size();
    complete_turn();
    return;
  }

  ++stats.rounds;
  turn.staged.push_back({.role = ChatRole::assistant,
                         .content = turn.round_text,
                         .tool_calls = finished->tool_calls});
  dispatch_round(stats.rounds, finished->tool_calls);
  if (auto started = start_round(); !started) {
    end_turn(TurnStatus::error, std::move(started.error()));
  }
}

void ToolLoopTransport::dispatch_round(const std::uint32_t round,
                                       const std::vector<ChatToolCall> &calls) {
  auto &turn = *turn_;
  auto &stats = turn.outcome.tool_stats;
  for (std::uint32_t index = 0; index < calls.size(); ++index) {
    const auto &call = calls[index];
    ++stats.calls;
    std::string content;
    if (turn.calls_spent >= limits_.max_tool_calls_per_turn) {
      ++stats.rejected_calls;
      content = tool_call_limit_result;
    } else {
      ++turn.calls_spent;
      auto result = dispatcher_.dispatch(
          {.round = round, .index = index, .call_id = call.id}, call.name,
          call.arguments_json);
      if (result.status == ToolDispatchStatus::refused) {
        ++stats.rejected_calls;
      } else if (result.status == ToolDispatchStatus::invalid) {
        ++stats.invalid_calls;
      }
      content = std::move(result.content);
    }
    turn.staged.push_back({.role = ChatRole::tool,
                           .content = std::move(content),
                           .tool_call_id = call.id});
  }
}

void ToolLoopTransport::complete_turn() {
  auto &staged = turn_->staged;
  history_.insert(history_.end(), std::make_move_iterator(staged.begin()),
                  std::make_move_iterator(staged.end()));
  staged.clear();
  end_turn(TurnStatus::completed, {});
}

void ToolLoopTransport::end_turn(const TurnStatus status, std::string error) {
  // Staged messages of a turn that did not complete are discarded here.
  auto turn = std::move(*turn_);
  turn_.reset();
  turn.outcome.status = status;
  turn.outcome.error = std::move(error);
  if (turn.callbacks.on_finished) {
    turn.callbacks.on_finished(std::move(turn.outcome));
  }
}

} // namespace pigpen::core
