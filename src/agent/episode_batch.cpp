/// @file episode_batch.cpp
/// @brief EpisodeBatch implementation; the contract is in the header.
#include "agent/episode_batch.hpp"

#include <algorithm>
#include <utility>

namespace pigpen::agent {

EpisodeBatch::Slot::Slot(const std::size_t job_index, BatchEntry started,
                         const Clock::duration timeout)
    : job(job_index), entry(std::move(started)),
      driver(*entry.episode, timeout) {}

EpisodeBatch::EpisodeBatch(const std::size_t job_count,
                           const std::size_t parallel,
                           const Clock::duration timeout, Factory factory)
    : job_count_(job_count), parallel_(std::max<std::size_t>(parallel, 1)),
      timeout_(timeout), factory_(std::move(factory)) {}

std::optional<BatchResult> EpisodeBatch::step(const Clock::time_point now,
                                              const bool stop_requested) {
  if (result_) {
    return result_;
  }
  if (!first_step_) {
    first_step_ = now;
  }
  progressed_ = false;
  stop_requested_ = stop_requested_ || stop_requested;

  // A factory failure stops the batch exactly as a stop request does.
  const auto stopping = [this] { return stop_requested_ || error_; };
  while (!stopping() && slots_.size() < parallel_ && next_job_ < job_count_) {
    const auto job = next_job_++;
    progressed_ = true;
    auto entry = factory_(job);
    if (!entry) {
      error_ = std::move(entry.error());
    } else if (!entry->episode) {
      error_ = "the episode factory returned no episode";
    } else {
      slots_.emplace_back(job, std::move(*entry), timeout_);
      ++started_;
    }
  }

  for (auto slot = slots_.begin(); slot != slots_.end();) {
    const auto outcome = slot->driver.step(now, stopping());
    if (!slot->driver.idle()) {
      progressed_ = true;
    }
    if (!outcome) {
      ++slot;
      continue;
    }
    progressed_ = true;
    if (slot->entry.on_end) {
      slot->entry.on_end({
          .job = slot->job,
          .outcome = *outcome,
          .timed_out = slot->driver.timed_out(),
      });
    }
    // Destroys the driver, then the entry and with it the episode.
    slot = slots_.erase(slot);
  }

  if (slots_.empty() && (stopping() || next_job_ == job_count_)) {
    result_ = BatchResult{
        .jobs = job_count_,
        .started = started_,
        .not_started = job_count_ - started_,
        .stopped = stop_requested_,
        .error = error_,
        .duration = now - *first_step_,
    };
  }
  return result_;
}

BatchResult EpisodeBatch::run(const BatchLoop &loop) {
  while (true) {
    if (auto result = step(loop.now(), loop.stop_requested())) {
      return *std::move(result);
    }
    if (idle()) {
      loop.sleep(idle_pause);
    }
  }
}

bool EpisodeBatch::idle() const noexcept { return !progressed_; }

std::size_t EpisodeBatch::live() const noexcept { return slots_.size(); }

} // namespace pigpen::agent
