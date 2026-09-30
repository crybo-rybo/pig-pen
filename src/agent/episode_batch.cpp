/// @file episode_batch.cpp
/// @brief EpisodeBatch implementation; the contract is in the header.
#include "agent/episode_batch.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief @p context and the in-flight exception's message; call only from
/// a catch block.
[[nodiscard]] std::string exception_message(std::string context) {
  try {
    throw;
  } catch (const std::exception &error) {
    return context + ": " + error.what();
  } catch (...) {
    return context + ": unknown exception";
  }
}

} // namespace

EpisodeBatch::Slot::Slot(const std::size_t job_index, BatchEntry started,
                         const Clock::duration timeout)
    : job(job_index), entry(std::move(started)),
      driver(*entry.episode, timeout) {}

EpisodeBatch::EpisodeBatch(const std::size_t job_count,
                           const std::size_t parallel,
                           const Clock::duration timeout, Factory factory)
    : EpisodeBatch(
          [job_count](const std::size_t next) {
            return next < job_count ? JobStatus::ready : JobStatus::exhausted;
          },
          parallel, timeout, std::move(factory)) {
  job_count_ = job_count;
  // Known up front, so the batch ends in the pass its last job ends.
  exhausted_ = job_count == 0;
}

EpisodeBatch::EpisodeBatch(JobSource source, const std::size_t parallel,
                           const Clock::duration timeout, Factory factory)
    : source_(std::move(source)), parallel_(std::max<std::size_t>(parallel, 1)),
      timeout_(timeout), factory_(std::move(factory)) {}

JobStatus EpisodeBatch::poll_source() {
  try {
    return source_(next_job_);
  } catch (...) {
    error_ =
        exception_message("could not read job " + std::to_string(next_job_));
    return JobStatus::pending;
  }
}

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

  // An error stops the batch exactly as a stop request does.
  const auto stopping = [this] { return stop_requested_ || error_; };
  while (!stopping() && !exhausted_ && slots_.size() < parallel_) {
    const auto status = poll_source();
    if (error_) {
      progressed_ = true;
      break;
    }
    if (status == JobStatus::exhausted) {
      exhausted_ = true;
      progressed_ = true;
      break;
    }
    if (status == JobStatus::pending) {
      break;
    }
    const auto job = next_job_++;
    exhausted_ = job_count_ && next_job_ == *job_count_;
    progressed_ = true;
    try {
      auto entry = factory_(job);
      if (!entry) {
        error_ = std::move(entry.error());
      } else if (!entry->episode) {
        error_ = "the episode factory returned no episode";
      } else {
        slots_.emplace_back(job, std::move(*entry), timeout_);
        ++started_;
      }
    } catch (...) {
      // Nothing was started, so there is nothing to report.
      error_ = exception_message("could not start job " + std::to_string(job));
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
      try {
        slot->entry.on_end({
            .job = slot->job,
            .outcome = *outcome,
            .timed_out = slot->driver.timed_out(),
        });
      } catch (...) {
        // The report was attempted; it is never retried. Stop the rest.
        if (!error_) {
          error_ = exception_message("could not report job " +
                                     std::to_string(slot->job));
        }
      }
    }
    // Destroys the driver, then the entry and with it the episode.
    slot = slots_.erase(slot);
  }

  if (slots_.empty() && (stopping() || exhausted_)) {
    const auto jobs = job_count_.value_or(next_job_);
    result_ = BatchResult{
        .jobs = jobs,
        .started = started_,
        .not_started = jobs - started_,
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
