/// @file turn_transport.hpp
/// @brief The "send one turn, get callbacks" seam between the episode
/// runner and any model transport.
///
/// EpisodeRunner depends only on this interface, which is what lets the
/// whole turn loop be tested against a scripted transport without a model.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace pigpen::core {

/// @brief How one model turn ended.
enum class TurnStatus : std::uint8_t {
  completed,
  cancelled,
  error,
};

/// @brief The tool loop's accounting for one turn, distinct from decoded
/// world actions. The counts so far are always known, also for a turn that
/// failed or was cancelled.
struct TurnToolStats {
  /// Rounds whose calls were dispatched.
  std::uint32_t rounds{};
  /// Every call the model requested in dispatched rounds, including calls
  /// over the budget.
  std::uint32_t calls{};
  /// Budget refusals and admission refusals.
  std::uint32_t rejected_calls{};
  /// Unknown tools and arguments that did not decode.
  std::uint32_t invalid_calls{};
  bool round_limit_reached{};
  /// Calls of the reply that hit the round limit; they never ran.
  std::size_t unexecuted_calls{};

  friend bool operator==(const TurnToolStats &,
                         const TurnToolStats &) = default;
};

/// @brief Terminal result of one model turn, delivered via
/// TurnCallbacks::on_finished.
struct TurnOutcome {
  TurnStatus status{TurnStatus::completed};
  /// The text of all rounds, as it was streamed.
  std::string text{};
  std::string error{};
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
  /// Prompt tokens that the backend reused from its KV cache.
  std::uint64_t cached_input_tokens{};
  /// Chat requests started for the turn; the failed one is included.
  std::uint32_t requests{};
  TurnToolStats tool_stats{};
};

/// @brief Streaming and completion callbacks for one turn.
/// @note on_finished is the terminal signal; exactly one outcome arrives per
/// accepted send, including after a cancellation. Callbacks never run inside
/// send(), and none for a turn arrive after its on_finished.
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
  virtual bool cancel() noexcept = 0;
};

} // namespace pigpen::core
