#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace pigpen::agent {

enum class TurnStatus : std::uint8_t {
  completed,
  cancelled,
  error,
};

struct TurnOutcome {
  TurnStatus status{TurnStatus::completed};
  std::string text{};
  std::string error{};
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
};

struct TurnCallbacks {
  std::function<void(std::string_view)> on_text_delta{};
  std::function<void(TurnOutcome)> on_finished{};
};

class ITurnTransport {
public:
  virtual ~ITurnTransport() = default;

  [[nodiscard]] virtual std::expected<void, std::string>
  send(std::string user_message, TurnCallbacks callbacks) = 0;
  [[nodiscard]] virtual bool cancel() noexcept = 0;
};

} // namespace pigpen::agent
