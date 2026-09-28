/// @file line_reader.cpp
/// @brief LineReader implementation; the contract is in the header.
#include "cli/line_reader.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

#if defined(_WIN32)
#include <climits>
#include <io.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace pigpen::cli {
namespace {

/// @brief Bytes asked of ReadBytes at a time.
constexpr std::size_t chunk_bytes{4096};

} // namespace

LineReader::LineReader(ReadBytes read, const std::size_t max_bytes,
                       const std::size_t capacity)
    : state_(std::make_shared<State>()),
      thread_(run, std::move(read), std::max<std::size_t>(max_bytes, 1),
              std::max<std::size_t>(capacity, 1), state_) {}

LineReader::~LineReader() {
  bool joinable = false;
  {
    const std::lock_guard lock{state_->mutex};
    state_->abandoned = true;
    // Outside a read, the thread is done or about to see abandoned.
    joinable = !state_->reading;
  }
  state_->space.notify_all();
  if (joinable) {
    thread_.join();
  } else {
    thread_.detach();
  }
}

LineStatus LineReader::poll(InputLine &line) {
  bool taken = false;
  bool closed = false;
  {
    const std::lock_guard lock{state_->mutex};
    if (!state_->lines.empty()) {
      line = std::move(state_->lines.front());
      state_->lines.pop_front();
      taken = true;
    }
    closed = state_->closed;
  }
  if (taken) {
    state_->space.notify_one();
    return LineStatus::line;
  }
  return closed ? LineStatus::closed : LineStatus::waiting;
}

void LineReader::run(const ReadBytes &read, const std::size_t max_bytes,
                     const std::size_t capacity,
                     const std::shared_ptr<State> &state) {
  // Waits for queue space, then queues @p complete; false once abandoned.
  const auto queue = [&](InputLine complete) {
    std::unique_lock lock{state->mutex};
    state->space.wait(lock, [&] {
      return state->abandoned || state->lines.size() < capacity;
    });
    if (state->abandoned) {
      return false;
    }
    state->lines.push_back(std::move(complete));
    return true;
  };

  std::array<char, chunk_bytes> chunk{};
  InputLine line{.number = 1};
  bool started = false;
  try {
    while (true) {
      {
        const std::lock_guard lock{state->mutex};
        if (state->abandoned) {
          return;
        }
        state->reading = true;
      }
      const auto count =
          std::min(read(chunk.data(), chunk.size()), chunk.size());
      {
        const std::lock_guard lock{state->mutex};
        state->reading = false;
        if (state->abandoned) {
          return;
        }
      }
      if (count == 0) {
        break;
      }
      for (const auto character : std::span{chunk}.first(count)) {
        started = true;
        if (character == '\n') {
          const auto next = line.number + 1;
          if (!queue(std::exchange(line, InputLine{.number = next}))) {
            return;
          }
          started = false;
        } else if (line.text.size() < max_bytes) {
          line.text.push_back(character);
        } else {
          line.truncated = true;
        }
      }
    }
    if (started && !queue(std::move(line))) {
      return;
    }
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // A source that fails by throwing ends the input like any other error.
  }
  const std::lock_guard lock{state->mutex};
  state->closed = true;
}

LineReader::ReadBytes standard_input_bytes() {
#if defined(_WIN32)
  return [](char *const data, const std::size_t size) -> std::size_t {
    const auto count = _read(
        0, data, static_cast<unsigned>(std::min<std::size_t>(size, INT_MAX)));
    return count > 0 ? static_cast<std::size_t>(count) : 0;
  };
#else
  return [](char *const data, const std::size_t size) -> std::size_t {
    while (true) {
      const auto count = ::read(STDIN_FILENO, data, size);
      if (count >= 0) {
        return static_cast<std::size_t>(count);
      }
      if (errno != EINTR) {
        return 0;
      }
    }
  };
#endif
}

} // namespace pigpen::cli
