/// @file episode_runner.hpp
/// @brief The episode state machine: idle -> playing <-> paused -> finished.
///
/// EpisodeRunner drives the turn loop over an ITurnTransport. Cancellation
/// is cooperative: stop() asks the transport to cancel and the episode is
/// not finished until the terminal callback comes back, which is what lets
/// the metrics footer be written before exit.
#pragma once

#include "agent/turn_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pigpen::agent {

/// @brief Lifecycle of one episode as driven by EpisodeRunner.
enum class RunState : std::uint8_t {
  idle,
  playing,
  paused,
  finished,
};

/// @brief Why an episode ended; recorded as `finish_reason` in the log
/// footer.
enum class FinishReason : std::uint8_t {
  turn_budget,
  objective_complete,
  stopped,
  cancelled,
  error,
};

/// @brief Who produced a transcript entry.
enum class TranscriptRole : std::uint8_t {
  automatic,
  guidance,
  assistant,
  error,
};

/// @brief Whether queued human guidance has been delivered to the model yet.
enum class GuidanceStatus : std::uint8_t {
  pending,
  sent,
};

/// @brief One labelled line of the episode transcript.
struct TranscriptEntry {
  std::uint32_t turn{};
  TranscriptRole role{TranscriptRole::automatic};
  std::string text{};
};

/// @brief Queued human guidance; delivered FIFO, one entry per model turn.
struct GuidanceEntry {
  std::uint64_t id{};
  /// Turn the entry was (or is predicted to be) sent on.
  std::uint32_t turn{};
  GuidanceStatus status{GuidanceStatus::pending};
  std::string text{};
};

/// @brief Everything known about one finished model turn.
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

/// @brief Terminal outcome delivered to EpisodeObservers::on_episode_finished.
struct EpisodeResult {
  FinishReason reason{FinishReason::turn_budget};
  std::uint32_t turns_used{};
  std::string error{};
};

/// @brief Point-in-time view of the runner for UIs, logging, and tools.
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

/// @brief Optional completion callbacks, invoked synchronously from tick().
struct EpisodeObservers {
  std::function<void(const TurnRecord &)> on_turn_finished{};
  std::function<void(const EpisodeResult &)> on_episode_finished{};
};

/// @brief Runs the turn loop of one episode against an ITurnTransport.
///
/// All state changes happen on the caller's thread inside tick(); transport
/// callbacks only stage an outcome that the next tick() consumes. This is
/// what lets the whole loop be tested against a scripted transport.
class EpisodeRunner final {
public:
  /// @brief Bind the runner to a transport and an episode configuration.
  /// @param turn_budget Clamped to at least one turn.
  /// @param objective_complete Optional predicate polled between turns; a
  /// true result finishes the episode with
  /// FinishReason::objective_complete.
  /// @param tool_call_count Optional counter used to attribute world-tool
  /// calls to turns and to detect zero-tool turns.
  EpisodeRunner(ITurnTransport &transport, std::uint32_t turn_budget,
                std::function<bool()> objective_complete,
                EpisodeObservers observers = {},
                std::function<std::size_t()> tool_call_count = {});
  /// @brief Cancels any in-flight turn; its late callbacks are ignored.
  ~EpisodeRunner();

  EpisodeRunner(const EpisodeRunner &) = delete;
  EpisodeRunner &operator=(const EpisodeRunner &) = delete;
  EpisodeRunner(EpisodeRunner &&) = delete;
  EpisodeRunner &operator=(EpisodeRunner &&) = delete;

  /// @brief Enter RunState::playing.
  /// @return false when already playing or finished.
  [[nodiscard]] bool play();
  /// @brief Pause between turns; the in-flight turn still completes.
  /// @return false unless currently playing without a pending stop.
  [[nodiscard]] bool pause();
  /// @brief Request cooperative shutdown.
  /// @note With a turn in flight this only asks the transport to cancel;
  /// the episode finishes on the terminal callback, not here.
  [[nodiscard]] bool stop();
  /// @brief Finish immediately with FinishReason::error.
  /// @return false when the episode is already finished.
  [[nodiscard]] bool fail(std::string error);
  /// @brief Advance the state machine: apply a staged turn outcome, then
  /// start the next turn when one is due. Call from the front end's loop.
  void tick();
  /// @brief Queue human guidance; sent FIFO, one entry per model turn.
  /// @return An id usable with remove_pending_user_input().
  [[nodiscard]] std::uint64_t queue_user_input(std::string message);
  /// @brief Remove one not-yet-sent guidance entry.
  /// @return false when the id is unknown or the entry was already sent.
  [[nodiscard]] bool remove_pending_user_input(std::uint64_t id);
  /// @brief Drop all guidance that has not been sent yet.
  void clear_pending_user_inputs();

  /// @brief Copy of the current episode state.
  [[nodiscard]] EpisodeSnapshot snapshot() const;
  /// @brief Full transcript, oldest first.
  [[nodiscard]] const std::vector<TranscriptEntry> &transcript() const noexcept;
  /// @brief All guidance entries, pending and sent.
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

/// @brief Stable lowercase name used in logs and the CLI.
[[nodiscard]] std::string_view run_state_name(RunState state) noexcept;
/// @brief Stable lowercase name used in logs and the CLI.
[[nodiscard]] std::string_view finish_reason_name(FinishReason reason) noexcept;

} // namespace pigpen::agent
