/// @file tool_loop.hpp
/// @brief ToolLoopTransport: one model turn as a loop of chat requests and
/// tool rounds, with the call budget, the round limit, and the history.
///
/// llamad is stateless and never runs a tool, and scrydantic only binds and
/// codes tools. This class is the agent loop between them: it resends the
/// whole conversation on each round, dispatches the calls of each reply, and
/// commits the messages of a turn only when the turn completes.
#pragma once

#include "core/chat_client.hpp"
#include "core/config.hpp"
#include "core/tool_dispatcher.hpp"
#include "core/turn_transport.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::core {

/// @brief The per-turn bounds of the tool loop.
struct ToolLoopLimits {
  /// Rounds whose calls are dispatched in one turn. A reply with calls after
  /// that completes the turn without running them.
  std::uint32_t max_tool_rounds{8};
  /// Calls dispatched in one turn, across rounds; invalid and refused calls
  /// also spend it.
  std::uint32_t max_tool_calls_per_turn{max_world_tool_calls_per_turn};
};

/// @brief The tool result for a call over the per-turn budget.
inline constexpr std::string_view tool_call_limit_result{
    R"({"error":"tool call limit for this turn reached; respond without calling tools"})"};

/// @brief ITurnTransport over an IChatClient and an IToolDispatcher.
///
/// All work happens on the thread that delivers the chat client's callbacks
/// (the pump thread). Tools run synchronously inside that delivery. The
/// client must outlive this object; destruction cancels an active request
/// and ignores anything that the client delivers later.
class ToolLoopTransport final : public ITurnTransport {
public:
  ToolLoopTransport(IChatClient &client, IToolDispatcher &dispatcher,
                    std::string system_prompt, ChatSampling sampling,
                    ToolLoopLimits limits);
  ~ToolLoopTransport() override;

  ToolLoopTransport(const ToolLoopTransport &) = delete;
  ToolLoopTransport &operator=(const ToolLoopTransport &) = delete;

  /// @brief Start one turn with @p user_message.
  /// @return An error while a turn is active, or when the first request
  /// cannot start; nothing is then staged.
  [[nodiscard]] std::expected<void, std::string>
  send(std::string user_message, TurnCallbacks callbacks) override;
  /// @brief Ask the active turn to stop; it ends with TurnStatus::cancelled
  /// unless its current reply already completed it.
  bool cancel() noexcept override;

  /// @brief The committed conversation, system message first.
  [[nodiscard]] const std::vector<ChatMessage> &history() const noexcept {
    return history_;
  }
  /// @brief Whether a turn has not delivered its on_finished yet.
  [[nodiscard]] bool turn_active() const noexcept { return turn_.has_value(); }

private:
  struct ActiveTurn {
    TurnCallbacks callbacks{};
    /// Messages of this turn, committed only when the turn completes.
    std::vector<ChatMessage> staged{};
    std::string round_text{};
    TurnOutcome outcome{};
    std::uint32_t calls_spent{};
    bool cancel_requested{};
  };

  [[nodiscard]] std::expected<void, std::string> start_round();
  void on_round_finished(std::expected<ChatFinish, ChatError> finished);
  void dispatch_round(std::uint32_t round,
                      const std::vector<ChatToolCall> &calls);
  void complete_turn();
  void end_turn(TurnStatus status, std::string error);

  IChatClient &client_;
  IToolDispatcher &dispatcher_;
  ChatSampling sampling_;
  ToolLoopLimits limits_;
  std::vector<ChatMessage> history_{};
  std::optional<ActiveTurn> turn_{};
  /// Callbacks hold a weak reference, so a late delivery after destruction
  /// does nothing.
  std::shared_ptr<ToolLoopTransport *> alive_;
};

} // namespace pigpen::core
