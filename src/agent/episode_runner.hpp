#pragma once

#include "turn_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

enum class RunState : std::uint8_t {
  idle,
  playing,
  paused,
  finished,
};

enum class FinishReason : std::uint8_t {
  turn_budget,
  objective_complete,
  stopped,
  cancelled,
  error,
};

enum class TranscriptRole : std::uint8_t {
  automatic,
  guidance,
  assistant,
  error,
};

enum class GuidanceStatus : std::uint8_t {
  pending,
  sent,
};

struct TranscriptEntry {
  std::uint32_t turn{};
  TranscriptRole role{TranscriptRole::automatic};
  std::string text{};
};

struct GuidanceEntry {
  std::uint64_t id{};
  std::uint32_t turn{};
  GuidanceStatus status{GuidanceStatus::pending};
  std::string text{};
};

struct TurnRecord {
  std::uint32_t turn{};
  TurnStatus status{TurnStatus::completed};
  std::string user_message{};
  std::string assistant_text{};
  std::string error{};
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
  std::size_t tool_calls{};
  std::chrono::milliseconds latency{};
};

struct EpisodeResult {
  FinishReason reason{FinishReason::turn_budget};
  std::uint32_t turns_used{};
  std::string error{};
};

struct EpisodeSnapshot {
  RunState state{RunState::idle};
  std::uint32_t turns_used{};
  std::uint32_t turn_budget{};
  bool turn_in_flight{false};
  bool stop_requested{false};
  std::optional<FinishReason> finish_reason{};
  std::string error{};
  std::chrono::milliseconds last_turn_latency{};
};

struct EpisodeObservers {
  std::function<void(const TurnRecord &)> on_turn_finished{};
  std::function<void(const EpisodeResult &)> on_episode_finished{};
};

class EpisodeRunner final {
public:
  EpisodeRunner(ITurnTransport &transport, std::uint32_t turn_budget,
                std::function<bool()> objective_complete,
                EpisodeObservers observers = {},
                std::function<std::size_t()> tool_call_count = {});
  ~EpisodeRunner();

  EpisodeRunner(const EpisodeRunner &) = delete;
  EpisodeRunner &operator=(const EpisodeRunner &) = delete;
  EpisodeRunner(EpisodeRunner &&) = delete;
  EpisodeRunner &operator=(EpisodeRunner &&) = delete;

  [[nodiscard]] bool play();
  [[nodiscard]] bool pause();
  [[nodiscard]] bool stop();
  [[nodiscard]] bool fail(std::string error);
  void tick();
  [[nodiscard]] std::uint64_t queue_user_input(std::string message);
  [[nodiscard]] bool remove_pending_user_input(std::uint64_t id);
  void clear_pending_user_inputs();

  [[nodiscard]] EpisodeSnapshot snapshot() const;
  [[nodiscard]] const std::vector<TranscriptEntry> &transcript() const noexcept;
  [[nodiscard]] const std::vector<GuidanceEntry> &guidance() const noexcept;

private:
  struct SharedState;

  void start_turn();
  void process_pending_outcome();
  void update_pending_guidance_turns();
  void finish(FinishReason reason, std::string error = {});

  ITurnTransport &transport_;
  std::function<bool()> objective_complete_;
  std::function<std::size_t()> tool_call_count_;
  EpisodeObservers observers_;
  std::shared_ptr<SharedState> state_;
};

[[nodiscard]] std::string_view run_state_name(RunState state) noexcept;
[[nodiscard]] std::string_view finish_reason_name(FinishReason reason) noexcept;

} // namespace pigpen::agent
