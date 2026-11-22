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

/// @brief The two time limits of a connection.
struct ConnectOptions {
  /// Time limit for the GetModelInfo call of connect(). The call does not
  /// wait for the daemon to start, so a missing socket fails at once; a
  /// daemon that accepts the connection but does not answer costs this long.
  std::chrono::milliseconds deadline{std::chrono::seconds{5}};
  /// The longest silence of an open Chat stream before pump() stops the
  /// request. Zero turns the watchdog off. While a request queues or runs,
  /// llamad sends a chunk at least about once a second plus the slowest
  /// single engine step, so a live daemon is never silent for long. This
  /// limit is on silence, not on the duration of a request: a request that
  /// waits in the daemon's queue behind another client gets progress
  /// chunks, and is not stopped.
  std::chrono::milliseconds stall_timeout{std::chrono::seconds{120}};
};

/// @brief A connection to one llamad daemon.
///
/// Threads: the caller's thread (the "pump thread") calls every member
/// function. Each request reads its stream on its own worker thread. The
/// worker only queues events and records signs of life, under the mutex of
/// the client; pump() delivers the events, so all callbacks run on the pump
/// thread. cancel() is safe from that thread at any time.
///
/// Stall watchdog: the worker records the time of the last sign of life of
/// the open stream (the start of the call, and every chunk that it reads,
/// also `progress` chunks and kinds that it does not know). pump() compares
/// it with ConnectOptions::stall_timeout. When the silence is longer, pump()
/// cancels the call and marks the request as stalled. The request then ends
/// through its normal on_finished, exactly once, with a
/// core::ChatErrorKind::failure (never `cancelled`) whose message says how
/// long llamad sent nothing. A cancel() before the watchdog fires wins: the
/// request ends as `cancelled`. A finish chunk that the daemon sent before
/// the watchdog took effect wins over both.
class LlamadChatClient final : public core::IChatClient {
public:
  /// @brief Open `unix:<socket_path>` and read the model information.
  /// @param options The GetModelInfo deadline and the stall timeout of each
  /// later request.
  /// @return The client, or a message when the daemon is unreachable (also
  /// when it does not answer within the deadline), is not of the llamad
  /// minor series whose proto the build pins (0.2.x), or serves a model that
  /// cannot chat (an embedding model, or no chat template).
  [[nodiscard]] static std::expected<std::unique_ptr<LlamadChatClient>,
                                     std::string>
  connect(std::string socket_path, ConnectOptions options = {});

  /// @brief Cancels an active request, joins its worker, and drops every
  /// event that was not delivered.
  ~LlamadChatClient() override;

  LlamadChatClient(const LlamadChatClient &) = delete;
  LlamadChatClient &operator=(const LlamadChatClient &) = delete;

  [[nodiscard]] const core::ModelInfo &model_info() const noexcept;
  [[nodiscard]] const std::string &socket_path() const noexcept;

  /// @return An error while the previous request has not delivered its
  /// on_finished, or when the worker thread cannot start. A failed start
  /// leaves the client idle and never runs a callback.
  [[nodiscard]] std::expected<void, std::string>
  start(core::ChatRequest request, core::ChatCallbacks callbacks) override;
  bool cancel() noexcept override;

  /// @brief Run the stall watchdog, then deliver at most @p max_callbacks
  /// queued events on this thread.
  ///
  /// The watchdog runs on each call, also with @p max_callbacks zero, so the
  /// caller must call pump() regularly while a request is active. A stall
  /// that pump() detects is delivered after the worker has queued the
  /// terminal event, normally by a later pump().
  [[nodiscard]] DeliveryStats pump(std::size_t max_callbacks = 32);

private:
  struct Impl;

  explicit LlamadChatClient(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::llamad
