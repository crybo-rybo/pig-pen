/// @file scry_transport.cpp
/// @brief ScryTurnTransport implementation; the contract is in the header.
#include "agent/scry_transport.hpp"

#include <utility>

namespace pigpen::agent {
namespace {

/// @brief Map a scry finish reason to an error message; empty means the
/// turn completed normally.
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

ScryTurnTransport::ScryTurnTransport(scry::Harness &harness,
                                     scry::Conversation &conversation)
    : harness_(harness), conversation_(conversation) {}

ScryTurnTransport::~ScryTurnTransport() {
  if (turn_) {
    static_cast<void>(turn_->cancel());
    // Nothing downstream of this transport outlives it, so drop delivery as
    // well: the turn still rolls its conversation back, but no callback can
    // reach a destroyed session.
    static_cast<void>(turn_->disconnect());
  }
}

std::expected<void, std::string>
ScryTurnTransport::send(std::string user_message, TurnCallbacks callbacks) {
  if (turn_ && !turn_->finished()) {
    return std::unexpected("a model turn is already active");
  }

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
              [callback = std::move(callbacks.on_finished)](
                  scry::Result<scry::Completion> finished) mutable {
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
  turn_.emplace(std::move(*result));
  return {};
}

bool ScryTurnTransport::cancel() noexcept {
  return turn_.has_value() && turn_->cancel();
}

} // namespace pigpen::agent
