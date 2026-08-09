/// @file scry_transport.hpp
/// @brief ITurnTransport implemented over scry::Harness and
/// scry::Conversation — the one real transport; tests use scripted ones.
#pragma once

#include "agent/turn_transport.hpp"

#include <scry/scry.hpp>

#include <memory>

namespace pigpen::agent {

/// @brief Sends one model turn at a time through a scry harness.
///
/// Callbacks are delivered from Harness::update() on the pump thread, never
/// concurrently. Shared state is held through a weak_ptr so a callback that
/// outlives the transport returns without touching freed memory.
class ScryTurnTransport final : public ITurnTransport {
public:
  ScryTurnTransport(scry::Harness &harness, scry::Conversation &conversation);
  /// @brief Cancels any turn still in flight.
  ~ScryTurnTransport() override;

  ScryTurnTransport(const ScryTurnTransport &) = delete;
  ScryTurnTransport &operator=(const ScryTurnTransport &) = delete;

  /// @brief Start one model turn.
  /// @return An error when a turn is already active or scry rejects the
  /// send; otherwise the outcome arrives via @p callbacks.
  [[nodiscard]] std::expected<void, std::string>
  send(std::string user_message, TurnCallbacks callbacks) override;
  /// @brief Ask scry to cancel the active turn.
  /// @return true when a cancellation was actually requested.
  /// @note Cooperative: the turn ends when its terminal callback arrives.
  [[nodiscard]] bool cancel() noexcept override;

private:
  struct State;

  scry::Harness &harness_;
  scry::Conversation &conversation_;
  std::shared_ptr<State> state_;
};

} // namespace pigpen::agent
