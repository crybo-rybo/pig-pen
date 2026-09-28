/// @file episode_driver.cpp
/// @brief EpisodeDriver implementation; the contract is in the header.
#include "agent/episode_driver.hpp"

#include <utility>

namespace pigpen::agent {

std::string_view drive_outcome_name(const DriveOutcome outcome) noexcept {
  switch (outcome) {
  case DriveOutcome::finished:
    return "finished";
  case DriveOutcome::timed_out:
    return "timed_out";
  case DriveOutcome::cancellation_stalled:
    return "cancellation_stalled";
  case DriveOutcome::interrupted:
    return "interrupted";
  }
  return "unknown";
}

EpisodeDriver::EpisodeDriver(IDrivableEpisode &episode,
                             const Clock::duration timeout,
                             EpisodeDriverObservers observers)
    : episode_(episode), timeout_(timeout), observers_(std::move(observers)) {}

std::optional<DriveOutcome> EpisodeDriver::step(const Clock::time_point now,
                                                const bool stop_requested) {
  if (outcome_) {
    return outcome_;
  }
  if (!deadline_) {
    deadline_ = now + timeout_;
  }
  if (stop_requested && !interrupted_) {
    interrupted_ = true;
    // Only a stop that precedes any timeout waits without limit; one that
    // arrives during the grace period keeps the grace deadline.
    waits_without_limit_ = !timed_out_;
    static_cast<void>(episode_.stop());
  }

  last_pump_ = episode_.pump();
  if (observers_.on_pumped) {
    observers_.on_pumped();
  }
  if (episode_.finished()) {
    outcome_ = interrupted_ ? DriveOutcome::interrupted
               : timed_out_ ? DriveOutcome::timed_out
                            : DriveOutcome::finished;
    return outcome_;
  }

  if (now >= *deadline_) {
    if (!timed_out_) {
      timed_out_ = true;
      deadline_ = now + cancellation_grace;
      if (observers_.on_timeout) {
        observers_.on_timeout();
      }
      static_cast<void>(episode_.stop());
    } else if (!waits_without_limit_) {
      // A stop requested first waits for its footer indefinitely; a timeout
      // gives up after the grace period so the caller stays scriptable, even
      // if a stop request arrives during it.
      outcome_ = DriveOutcome::cancellation_stalled;
    }
  }
  return outcome_;
}

std::optional<DriveOutcome> EpisodeDriver::outcome() const noexcept {
  return outcome_;
}

bool EpisodeDriver::idle() const noexcept {
  return last_pump_.callbacks_delivered == 0 &&
         last_pump_.events_remaining == 0;
}

bool EpisodeDriver::timed_out() const noexcept { return timed_out_; }

bool EpisodeDriver::interrupted() const noexcept { return interrupted_; }

} // namespace pigpen::agent
