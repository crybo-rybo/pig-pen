#include "scry_transport.hpp"

#include <optional>
#include <utility>

namespace pigpen::agent {
namespace {

[[nodiscard]] std::string finish_reason_error(const scry::FinishReason reason) {
  switch (reason) {
  case scry::FinishReason::completed:
    return {};
  case scry::FinishReason::length:
    return "model response reached its output-token limit";
  case scry::FinishReason::tool_use:
    return "model response ended with an unresolved tool request";
  case scry::FinishReason::unknown:
    return "model response ended for an unknown reason";
  }
  return "model response ended abnormally";
}

} // namespace

struct ScryTurnTransport::State {
  std::optional<scry::Turn> turn{};
  bool active{false};
};

ScryTurnTransport::ScryTurnTransport(scry::Harness &harness,
                                     scry::Conversation &conversation)
    : harness_(harness), conversation_(conversation),
      state_(std::make_shared<State>()) {}

ScryTurnTransport::~ScryTurnTransport() {
  if (state_->active && state_->turn) {
    static_cast<void>(state_->turn->cancel());
  }
  state_.reset();
}

std::expected<void, std::string>
ScryTurnTransport::send(std::string user_message, TurnCallbacks callbacks) {
  if (state_->active) {
    return std::unexpected("a model turn is already active");
  }

  const std::weak_ptr<State> weak_state{state_};
  auto result = harness_.send(
      conversation_, std::move(user_message),
      scry::TurnCallbacks{
          .on_text_delta =
              [callback = std::move(callbacks.on_text_delta)](
                  const std::string_view delta) mutable {
                if (callback) {
                  callback(delta);
                }
              },
          .on_finished =
              [weak_state, callback = std::move(callbacks.on_finished)](
                  scry::Result<scry::Completion> finished) mutable {
                const auto state = weak_state.lock();
                if (!state) {
                  return;
                }
                state->active = false;
                if (!callback) {
                  return;
                }
                if (!finished) {
                  callback({
                      .status = finished.error().category ==
                                        scry::ErrorCategory::cancelled
                                    ? TurnStatus::cancelled
                                    : TurnStatus::error,
                      .text = {},
                      .error = std::move(finished.error().message),
                  });
                  return;
                }
                const auto reason_error =
                    finish_reason_error(finished->finish_reason);
                callback({
                    .status = reason_error.empty() ? TurnStatus::completed
                                                   : TurnStatus::error,
                    .text = std::move(finished->text),
                    .error = reason_error,
                    .input_tokens = finished->usage.input_tokens,
                    .output_tokens = finished->usage.output_tokens,
                });
              },
      });
  if (!result) {
    return std::unexpected(result.error().message);
  }
  state_->turn.emplace(std::move(*result));
  state_->active = true;
  return {};
}

bool ScryTurnTransport::cancel() noexcept {
  return state_->active && state_->turn && state_->turn->cancel();
}

} // namespace pigpen::agent
