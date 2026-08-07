#pragma once

#include "turn_transport.hpp"

#include <memory>
#include <scry/scry.hpp>

namespace pigpen::agent {

class ScryTurnTransport final : public ITurnTransport {
public:
  ScryTurnTransport(scry::Harness &harness, scry::Conversation &conversation);
  ~ScryTurnTransport() override;

  ScryTurnTransport(const ScryTurnTransport &) = delete;
  ScryTurnTransport &operator=(const ScryTurnTransport &) = delete;

  [[nodiscard]] std::expected<void, std::string>
  send(std::string user_message, TurnCallbacks callbacks) override;
  [[nodiscard]] bool cancel() noexcept override;

private:
  struct State;

  scry::Harness &harness_;
  scry::Conversation &conversation_;
  std::shared_ptr<State> state_;
};

} // namespace pigpen::agent
