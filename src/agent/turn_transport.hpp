/// @file turn_transport.hpp
/// @brief The "send one turn, get callbacks" seam between the episode
/// runner and any model transport.
///
/// EpisodeRunner depends only on this interface, which is what lets the
/// whole turn loop be tested against a scripted transport without a model.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace pigpen::agent {

/// @brief How one model turn ended.
enum class TurnStatus : std::uint8_t {
  completed,
  cancelled,
  error,
};

/// @brief Terminal result of one model turn, delivered via
/// TurnCallbacks::on_finished.
struct TurnOutcome {
  TurnStatus status{TurnStatus::completed};
  std::string text{};
  std::string error{};
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
};

/// @brief Streaming and completion callbacks for one turn.
/// @note on_finished is the terminal signal; exactly one outcome arrives per
/// accepted send, including after a cancellation.
struct TurnCallbacks {
  std::function<void(std::string_view)> on_text_delta{};
  std::function<void(TurnOutcome)> on_finished{};
};

/// @brief One-turn-at-a-time model transport.
class ITurnTransport {
public:
  virtual ~ITurnTransport() = default;

  /// @brief Start one model turn.
  /// @return An error when the turn cannot be started; otherwise the
  /// outcome arrives asynchronously through @p callbacks.
  [[nodiscard]] virtual std::expected<void, std::string>
  send(std::string user_message, TurnCallbacks callbacks) = 0;
  /// @brief Request cooperative cancellation of the active turn.
  /// @return true when a cancellation was actually requested.
  [[nodiscard]] virtual bool cancel() noexcept = 0;
};

} // namespace pigpen::agent
