/// @file scry_transport.cpp
/// @brief ScryTurnTransport implementation; the contract is in the header.
#include "agent/scry_transport.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief Map a scry finish reason to an error message; empty means the
/// turn completed normally.
[[nodiscard]] std::string finish_reason_error(const scry::FinishReason reason) {
  switch (reason) {
  case scry::FinishReason::completed:
  case scry::FinishReason::tool_round_limit:
    return {};
  case scry::FinishReason::length:
    return "model response reached its output-token limit";
  case scry::FinishReason::tool_use:
    return "model response ended with an unresolved tool request";
  case scry::FinishReason::unknown:
    break;
  }
  return "model response ended for an unknown reason";
}

/// @brief Scry's generic message plus the sanitized provider diagnostics it
/// carries beside it, e.g. "provider rejected the request (HTTP 404,
/// openai:model_not_found)".
[[nodiscard]] std::string describe(scry::Error error) {
  std::string details;
  const auto add = [&details](const std::string_view detail) {
    details += details.empty() ? " (" : ", ";
    details += detail;
  };
  if (error.http_status != 0) {
    add("HTTP " + std::to_string(error.http_status));
  }
  if (!error.provider_detail.empty()) {
    add(error.provider_detail);
  }
  if (!error.provider_request_id.empty()) {
    add("request " + error.provider_request_id);
  }
  return details.empty() ? std::move(error.message)
                         : std::move(error.message) + details + ")";
}

[[nodiscard]] core::TurnOutcome
to_outcome(scry::Result<scry::Completion> finished) {
  if (!finished) {
    const auto status =
        finished.error().category == scry::ErrorCategory::cancelled
            ? core::TurnStatus::cancelled
            : core::TurnStatus::error;
    const auto attempts = finished.error().attempt;
    return {
        .status = status,
        .error = describe(std::move(finished.error())),
        .attempts = attempts,
    };
  }
  auto reason_error = finish_reason_error(finished->finish_reason);
  return {
      .status = reason_error.empty() ? core::TurnStatus::completed
                                     : core::TurnStatus::error,
      .text = std::move(finished->text),
      .error = std::move(reason_error),
      .input_tokens = finished->usage.input_tokens,
      .output_tokens = finished->usage.output_tokens,
      .attempts = finished->attempt_count,
      .tool_stats =
          core::TurnToolStats{
              .rounds = finished->tool_round_count,
              .calls = finished->tool_call_count,
              .rejected_calls = finished->rejected_tool_call_count,
              .round_limit_reached = finished->finish_reason ==
                                     scry::FinishReason::tool_round_limit,
              .unexecuted_calls = finished->unexecuted_tool_calls.size(),
          },
  };
}

/// @brief An empty std::function becomes an empty Scry callback, so Scry
/// skips that delivery instead of calling a wrapper that does nothing.
template <typename Signature>
[[nodiscard]] scry::UniqueFunction<Signature>
optional_callback(std::function<Signature> callback) {
  if (!callback) {
    return nullptr;
  }
  return std::move(callback);
}

} // namespace

scry::Config scry_config(const core::Config &config, std::string api_key) {
  return {
      .base_url = config.base_url,
      .api_key = std::move(api_key),
      .model = config.model,
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling = {.temperature = config.temperature,
                   .max_tokens = config.max_output_tokens,
                   .seed = config.sampling_seed},
      .reasoning_mode = scry::ReasoningMode::disabled,
      .max_tool_rounds = config.max_tool_rounds,
      .max_tool_calls_per_turn = core::max_world_tool_calls_per_turn,
      .tool_round_limit = scry::ToolRoundLimitPolicy::complete,
  };
}

ScryTurnTransport::ScryTurnTransport(scry::Harness &harness,
                                     scry::Conversation &conversation,
                                     ScryToolObservers observers)
    : harness_(harness), conversation_(conversation),
      observers_(std::move(observers)) {}

ScryTurnTransport::~ScryTurnTransport() {
  if (turn_) {
    turn_->cancel();
    // Nothing downstream of this transport outlives it, so drop delivery as
    // well: the turn still rolls its conversation back, but no callback can
    // reach a destroyed session.
    turn_->disconnect();
  }
}

std::expected<void, std::string>
ScryTurnTransport::send(std::string user_message,
                        core::TurnCallbacks callbacks) {
  // Scry clears Conversation::busy() once it ingests the terminal event, which
  // can precede delivery of on_finished under a limited callback budget.
  // Replacing turn_ then would leave the old callback, which captures this,
  // connected past destruction; wait for the handle to report finished.
  if (turn_ && !turn_->finished()) {
    return std::unexpected("a model turn is already active");
  }

  auto result = harness_.send(
      conversation_, std::move(user_message),
      {
          .on_text_delta =
              optional_callback(std::move(callbacks.on_text_delta)),
          // The observers serve every turn, so each turn gets copies.
          .on_tool_request = optional_callback(observers_.on_tool_request),
          .on_tool_call = optional_callback(observers_.on_tool_call),
          .on_finished =
              [this, callback = std::move(callbacks.on_finished)](
                  scry::Result<scry::Completion> finished) mutable {
                if (observers_.on_turn_finished) {
                  observers_.on_turn_finished();
                }
                if (callback) {
                  callback(to_outcome(std::move(finished)));
                }
              },
      });
  if (!result) {
    return std::unexpected(describe(std::move(result.error())));
  }
  turn_.emplace(std::move(*result));
  return {};
}

bool ScryTurnTransport::cancel() noexcept {
  return turn_.has_value() && turn_->cancel();
}

} // namespace pigpen::agent
