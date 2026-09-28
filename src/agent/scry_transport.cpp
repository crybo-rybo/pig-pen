/// @file scry_transport.cpp
/// @brief ScryTurnTransport implementation; the contract is in the header.
#include "agent/scry_transport.hpp"

#include <string>
#include <utility>
#include <vector>

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

} // namespace

scry::Config scry_config(const Config &config, const SessionOptions &options) {
  std::vector<scry::HttpHeader> headers;
  headers.reserve(options.request_headers.size() + 1);
  for (const auto &[name, value] : options.request_headers) {
    headers.push_back({.name = name, .value = value});
  }
  if (!options.rollout_id.empty()) {
    headers.push_back({.name = std::string{rollout_header_name},
                       .value = options.rollout_id});
  }
  return {
      .base_url = config.base_url,
      .api_key = options.api_key,
      .model = config.model,
      .dialect = scry::ProviderDialect::openai_compatible,
      .sampling = {.temperature = config.temperature,
                   .max_tokens = config.max_output_tokens,
                   .seed = config.sampling_seed},
      .reasoning_mode = scry::ReasoningMode::disabled,
      .max_tool_rounds = config.max_tool_rounds,
      .max_tool_calls_per_turn = max_world_tool_calls_per_turn,
      .tool_round_limit = scry::ToolRoundLimitPolicy::complete,
      .extra_headers = std::move(headers),
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
ScryTurnTransport::send(std::string user_message, TurnCallbacks callbacks) {
  // Scry clears Conversation::busy() once it ingests the terminal event, which
  // can precede delivery of on_finished under a limited callback budget.
  // Replacing turn_ then would leave the old callback, which captures this,
  // connected past destruction; wait for the handle to report finished.
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
          .on_tool_request =
              [this](const scry::ToolRequest &request) {
                return observers_.on_tool_request
                           ? observers_.on_tool_request(request)
                           : std::nullopt;
              },
          .on_tool_call =
              [this](const scry::ToolCall &call) {
                if (observers_.on_tool_call) {
                  observers_.on_tool_call(call);
                }
              },
          .on_finished =
              [this, callback = std::move(callbacks.on_finished)](
                  scry::Result<scry::Completion> finished) mutable {
                if (observers_.on_turn_finished) {
                  observers_.on_turn_finished();
                }
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
                    .tool_stats =
                        TurnToolStats{
                            .rounds = finished->tool_round_count,
                            .calls = finished->tool_call_count,
                            .rejected_calls =
                                finished->rejected_tool_call_count,
                            .round_limit_reached =
                                finished->finish_reason ==
                                scry::FinishReason::tool_round_limit,
                            .unexecuted_calls =
                                finished->unexecuted_tool_calls.size(),
                        },
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
