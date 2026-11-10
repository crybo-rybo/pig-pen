/// @file fake_llamad.hpp
/// @brief An in-process, scripted llamad service on a Unix socket, for tests.
///
/// The header is gRPC-free: replies are scripted and requests are recorded as
/// plain structs. No model and no network are used; the socket lives in a
/// short temporary directory because AF_UNIX paths are limited to about 107
/// bytes.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::testing {

/// @brief A short, unique temporary directory, removed on destruction.
class TempSocketDirectory final {
public:
  TempSocketDirectory();
  ~TempSocketDirectory();
  TempSocketDirectory(const TempSocketDirectory &) = delete;
  TempSocketDirectory &operator=(const TempSocketDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  /// @brief `<directory>/s.sock`.
  [[nodiscard]] std::string socket() const;

private:
  std::filesystem::path path_;
};

/// @brief The llamad FinishReason values, without the wire prefix.
enum class FakeFinishReason : std::uint8_t {
  eog,
  length,
  stop,
  cancelled,
  tool_calls,
  /// FINISH_REASON_UNSPECIFIED, which a real daemon never sends.
  unspecified,
};

struct FakeToolCall {
  std::string id{};
  std::string name{};
  std::string arguments_json{"{}"};

  friend bool operator==(const FakeToolCall &, const FakeToolCall &) = default;
};

/// @brief One scripted reply to one Chat call.
struct FakeReply {
  /// Text chunks, written first, in order.
  std::vector<std::string> text{};
  /// The finish chunk; nothing means the stream ends without one.
  std::optional<FakeFinishReason> finish{FakeFinishReason::eog};
  std::vector<FakeToolCall> tool_calls{};
  std::int32_t prompt_tokens{};
  std::int32_t completion_tokens{};
  std::int32_t cached_prompt_tokens{};
  /// A gRPC status code; non-zero ends the stream with that status and no
  /// finish chunk.
  int status_code{};
  std::string status_message{};
  /// After the text chunks, wait until the client cancels, then end with
  /// CANCELLED.
  bool hang{};

  /// @brief A reply that finishes with tool calls.
  [[nodiscard]] static FakeReply calls(std::vector<FakeToolCall> tool_calls,
                                       std::vector<std::string> text = {});
  /// @brief A text reply that finishes with EOG.
  [[nodiscard]] static FakeReply say(std::string text);
};

/// @brief What GetModelInfo reports.
struct FakeModelInfo {
  std::string description{"fake pig 1B Q4_K - Medium"};
  std::uint64_t n_params{1'000'000'000};
  std::uint64_t size_bytes{700'000'000};
  std::uint32_t n_ctx{32'768};
  std::uint32_t n_ctx_train{131'072};
  bool has_chat_template{true};
  bool serves_embeddings{false};
};

struct ReceivedMessage {
  std::string role{};
  std::string content{};
  std::vector<FakeToolCall> tool_calls{};
  std::string tool_call_id{};
};

struct ReceivedTool {
  std::string name{};
  std::string description{};
  std::string parameters_json_schema{};
};

/// @brief One Chat request as the fake received it.
struct ReceivedRequest {
  std::vector<ReceivedMessage> messages{};
  std::vector<ReceivedTool> tools{};
  std::optional<float> temperature{};
  std::optional<std::uint32_t> seed{};
  std::optional<std::int32_t> max_tokens{};
  std::optional<std::int32_t> top_k{};
  std::string response_json_schema{};
};

/// @brief The running fake daemon. Destruction shuts the server down and
/// cancels calls that still hang.
class FakeLlamad final {
public:
  /// @param on_request Called on a server thread for each Chat call, before
  /// the reply is written.
  /// @return The server, or nothing when it cannot listen on @p socket_path.
  [[nodiscard]] static std::unique_ptr<FakeLlamad>
  start(const std::string &socket_path, FakeModelInfo info = {},
        std::function<void(const ReceivedRequest &)> on_request = {});
  ~FakeLlamad();
  FakeLlamad(const FakeLlamad &) = delete;
  FakeLlamad &operator=(const FakeLlamad &) = delete;

  /// @brief Add the reply to the next Chat call. A call without a scripted
  /// reply ends with INTERNAL.
  void enqueue(FakeReply reply);
  [[nodiscard]] std::vector<ReceivedRequest> requests() const;
  /// @brief Wait until @p count Chat calls arrived.
  [[nodiscard]] bool wait_for_requests(
      std::size_t count,
      std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;
  /// @brief Wait until no Chat call is in progress.
  [[nodiscard]] bool wait_until_idle(
      std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;

private:
  struct Impl;

  explicit FakeLlamad(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::testing
