/// @file tool_dispatcher.hpp
/// @brief The seam between the tool loop and the tools that it runs.
///
/// ToolLoopTransport owns rounds, budgets, and history. A dispatcher owns
/// the tool contract (names, schemas, decode, encode) and admission. The real
/// dispatcher is agent::WorldToolDispatcher; the tests use scripted ones.
#pragma once

#include "core/chat_client.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pigpen::core {

/// @brief The identity of one tool call inside a turn.
struct ToolCallContext {
  /// Starts at one.
  std::uint32_t round{};
  /// Starts at zero in the batch of the round.
  std::uint32_t index{};
  std::string_view call_id{};
};

/// @brief What happened to one dispatched call.
enum class ToolDispatchStatus : std::uint8_t {
  /// The tool ran.
  executed,
  /// Admission refused the call (objective complete, log failed).
  refused,
  /// Unknown tool, or arguments that do not decode.
  invalid,
};

/// @brief The result of one dispatched call.
struct ToolDispatchResult {
  ToolDispatchStatus status{ToolDispatchStatus::executed};
  /// The exact text of the `tool` message that answers the call.
  std::string content{};
};

/// @brief Runs tool calls synchronously on the pump thread.
class IToolDispatcher {
public:
  virtual ~IToolDispatcher() = default;

  /// @brief The tools to offer on every request, in a stable order.
  [[nodiscard]] virtual const std::vector<ChatTool> &tools() const = 0;
  /// @brief Run one call; always returns the text for the model.
  [[nodiscard]] virtual ToolDispatchResult
  dispatch(const ToolCallContext &context, std::string_view name,
           std::string_view arguments_json) = 0;
};

} // namespace pigpen::core
