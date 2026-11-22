/// @file chat_client.hpp
/// @brief The one-chat-request seam between the tool loop and a model
/// backend, as plain values that mirror the llamad wire contract.
///
/// ToolLoopTransport depends only on this interface. The real implementation
/// is llamad::LlamadChatClient; the tests use scripted ones. Nothing here
/// parses or writes JSON: schemas and arguments are opaque text.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::core {

/// @brief The author of one conversation message.
enum class ChatRole : std::uint8_t {
  system,
  user,
  assistant,
  tool,
};

/// @brief One tool call as the model produced it, with complete arguments.
struct ChatToolCall {
  /// Assigned by the backend; a `tool` message echoes it.
  std::string id{};
  std::string name{};
  /// A complete JSON object, exactly as the backend sent it.
  std::string arguments_json{"{}"};

  friend bool operator==(const ChatToolCall &, const ChatToolCall &) = default;
};

/// @brief One message of the conversation that a request carries.
struct ChatMessage {
  ChatRole role{ChatRole::user};
  /// The text, or for a `tool` message the result as an opaque string.
  std::string content{};
  /// An assistant message's calls, replayed from history.
  std::vector<ChatToolCall> tool_calls{};
  /// A `tool` message: the id of the call that this result answers.
  std::string tool_call_id{};

  friend bool operator==(const ChatMessage &, const ChatMessage &) = default;
};

/// @brief A tool offered to the model for one request.
struct ChatTool {
  std::string name{};
  std::string description{};
  /// JSON Schema object for the arguments.
  std::string parameters_json_schema{};

  friend bool operator==(const ChatTool &, const ChatTool &) = default;
};

/// @brief Sampling settings that every request of an episode carries.
struct ChatSampling {
  double temperature{0.0};
  std::uint32_t max_tokens{8'192};
  /// Sent only when set; repeatability is best-effort.
  std::optional<std::uint32_t> seed{};

  friend bool operator==(const ChatSampling &, const ChatSampling &) = default;
};

/// @brief Whether a reasoning model writes reasoning before its reply.
enum class ChatReasoning : std::uint8_t {
  /// What the model's chat template does on its own. A reasoning model then
  /// streams its reasoning as ordinary text.
  model_default,
  /// Ask the chat template to turn reasoning off. A template without
  /// reasoning ignores it.
  off,
};

/// @brief One complete, stateless chat request.
struct ChatRequest {
  /// The whole conversation, in order; never empty.
  std::vector<ChatMessage> messages{};
  std::vector<ChatTool> tools{};
  ChatSampling sampling{};
  /// Always `off` in Pig Pen; no option changes it. The system prompt tells
  /// the model not to provide hidden chain-of-thought, and reasoning text
  /// would go into the transcript, the log, and the history that each
  /// request sends again.
  ChatReasoning reasoning{ChatReasoning::off};
};

/// @brief Why one generation ended.
enum class ChatFinishReason : std::uint8_t {
  end_of_generation,
  length,
  stop,
  tool_calls,
};

/// @brief Token counts and timings of one generation.
struct ChatStats {
  std::uint64_t prompt_tokens{};
  std::uint64_t completion_tokens{};
  /// Leading prompt tokens reused from the backend's KV cache.
  std::uint64_t cached_prompt_tokens{};
  /// Time the backend spent decoding the prompt tokens that were not cached.
  double prompt_ms{};
  /// Time the backend spent generating, streaming included.
  double completion_ms{};
};

/// @brief The terminal chunk of a successful generation.
struct ChatFinish {
  ChatFinishReason reason{ChatFinishReason::end_of_generation};
  ChatStats stats{};
  /// Non-empty only for ChatFinishReason::tool_calls.
  std::vector<ChatToolCall> tool_calls{};
};

/// @brief Why a request ended without a finish chunk.
enum class ChatErrorKind : std::uint8_t {
  /// Any failure that is not one of the kinds below.
  failure,
  /// The request ended because of a local cancel().
  cancelled,
  /// The backend refused the request because the prompt does not fit in its
  /// context (llamad `OUT_OF_RANGE`).
  prompt_too_long,
};

/// @brief A request that ended without a finish chunk. A cancelled request
/// always ends here, never with a finish chunk.
struct ChatError {
  ChatErrorKind kind{ChatErrorKind::failure};
  /// For a backend failure, the backend's status and message.
  std::string message{};
};

/// @brief Streaming and completion callbacks for one request.
/// @note Callbacks never run inside start(). Exactly one on_finished arrives
/// for each accepted start(), also after a cancel(), and nothing for that
/// request arrives after it. Destroying the client suppresses delivery.
struct ChatCallbacks {
  std::function<void(std::string_view)> on_text{};
  std::function<void(std::expected<ChatFinish, ChatError>)> on_finished{};
};

/// @brief What the backend reports about the one model that it serves.
struct ModelInfo {
  /// For example "qwen2 0.5B Q4_K - Medium".
  std::string description{};
  /// The model's own name (GGUF `general.name`); empty when it has none.
  std::string name{};
  /// The file name of the loaded model, without its directory.
  std::string file{};
  std::uint64_t n_params{};
  std::uint64_t size_bytes{};
  /// Context size the backend runs with, in tokens.
  std::uint32_t n_ctx{};
  /// Context size the model was trained with.
  std::uint32_t n_ctx_train{};
  /// The version of the daemon, for example "0.2.0".
  std::string daemon_version{};
  /// The largest serialized request that the daemon accepts.
  std::uint32_t max_request_bytes{};

  friend bool operator==(const ModelInfo &, const ModelInfo &) = default;
};

/// @brief One-request-at-a-time chat backend.
class IChatClient {
public:
  virtual ~IChatClient() = default;

  /// @brief Start one chat request.
  /// @return An error when the request cannot start, for example while
  /// another request has not delivered its on_finished.
  [[nodiscard]] virtual std::expected<void, std::string>
  start(ChatRequest request, ChatCallbacks callbacks) = 0;
  /// @brief Request cooperative cancellation of the active request.
  /// @return true when a cancellation was actually requested.
  virtual bool cancel() noexcept = 0;
};

/// @brief Stable lowercase name of @p role, as the llamad wire spells it.
[[nodiscard]] constexpr std::string_view
chat_role_name(const ChatRole role) noexcept {
  switch (role) {
  case ChatRole::system:
    return "system";
  case ChatRole::user:
    return "user";
  case ChatRole::assistant:
    return "assistant";
  case ChatRole::tool:
    return "tool";
  }
  return "unknown";
}

} // namespace pigpen::core
