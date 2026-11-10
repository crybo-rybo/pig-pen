/// @file llamad_client.hpp
/// @brief core::IChatClient over llamad's gRPC service on a Unix socket.
///
/// This header is gRPC-free (pimpl), so the code that includes it does not
/// see a gRPC or Protobuf header. Only pigpen_llamad links the generated
/// stubs.
#pragma once

#include "core/chat_client.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace pigpen::llamad {

/// @brief What one pump() pass delivered.
struct DeliveryStats {
  std::size_t callbacks_delivered{};
  std::size_t events_remaining{};
};

/// @brief A connection to one llamad daemon.
///
/// Each request reads its stream on its own worker thread. The worker only
/// queues events; pump() delivers them, so all callbacks run on the thread
/// that calls pump(). cancel() is safe from that thread at any time.
class LlamadChatClient final : public core::IChatClient {
public:
  /// @brief Open `unix:<socket_path>` and read the model information.
  /// @param deadline Time limit for the GetModelInfo call. The call does not
  /// wait for the daemon to start, so a missing socket fails at once.
  /// @return The client, or a message when the daemon is unreachable or its
  /// model cannot chat (an embedding model, or no chat template).
  [[nodiscard]] static std::expected<std::unique_ptr<LlamadChatClient>,
                                     std::string>
  connect(std::string socket_path,
          std::chrono::milliseconds deadline = std::chrono::seconds{5});

  /// @brief Cancels an active request, joins its worker, and drops every
  /// event that was not delivered.
  ~LlamadChatClient() override;

  LlamadChatClient(const LlamadChatClient &) = delete;
  LlamadChatClient &operator=(const LlamadChatClient &) = delete;

  [[nodiscard]] const core::ModelInfo &model_info() const noexcept;
  [[nodiscard]] const std::string &socket_path() const noexcept;

  /// @return An error while the previous request has not delivered its
  /// on_finished.
  [[nodiscard]] std::expected<void, std::string>
  start(core::ChatRequest request, core::ChatCallbacks callbacks) override;
  bool cancel() noexcept override;

  /// @brief Deliver at most @p max_callbacks queued events on this thread.
  [[nodiscard]] DeliveryStats pump(std::size_t max_callbacks = 32);

private:
  struct Impl;

  explicit LlamadChatClient(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::llamad
