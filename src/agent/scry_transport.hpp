/// @file scry_transport.hpp
/// @brief ITurnTransport implemented over scry::Harness and
/// scry::Conversation — the one real transport; tests use scripted ones.
#pragma once

#include "agent/config.hpp"
#include "agent/turn_transport.hpp"

#include <scry/scry.hpp>

#include <optional>

namespace pigpen::agent {

/// Pig Pen's provider and turn policies, shared with scripted integration
/// tests.
[[nodiscard]] scry::Config scry_config(const Config &config,
                                       std::string api_key = {});

struct ScryToolObservers {
  std::function<std::optional<scry::ToolRejection>(const scry::ToolRequest &)>
      on_tool_request{};
  std::function<void(const scry::ToolCall &)> on_tool_call{};
  std::function<void()> on_turn_finished{};
};

/// @brief Sends one model turn at a time through a scry harness.
///
/// Callbacks are delivered from Harness::update() on the pump thread, never
/// concurrently. The scry::Turn handle is the whole state: it reports whether
/// a turn is still live, and destruction cancels and disconnects it so no
/// callback can outlive this transport.
class ScryTurnTransport final : public ITurnTransport {
public:
  ScryTurnTransport(scry::Harness &harness, scry::Conversation &conversation,
                    ScryToolObservers observers = {});
  /// @brief Cancels any turn still in flight and stops its delivery.
  ~ScryTurnTransport() override;

  ScryTurnTransport(const ScryTurnTransport &) = delete;
  ScryTurnTransport &operator=(const ScryTurnTransport &) = delete;

  [[nodiscard]] std::expected<void, std::string>
  send(std::string user_message, TurnCallbacks callbacks) override;
  bool cancel() noexcept override;

private:
  scry::Harness &harness_;
  scry::Conversation &conversation_;
  ScryToolObservers observers_;
  std::optional<scry::Turn> turn_{};
};

} // namespace pigpen::agent
