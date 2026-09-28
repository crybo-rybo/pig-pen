/// @file line_reader.cpp
/// @brief LineReader implementation; the contract is in the header.
#include "cli/line_reader.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <system_error>
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

std::string LineReader::error() const {
  const std::lock_guard lock{state_->mutex};
  return state_->error;
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
  std::string error;
  try {
    while (true) {
      {
        const std::lock_guard lock{state->mutex};
        if (state->abandoned) {
          return;
        }
        state->reading = true;
      }
      const auto result = read(chunk.data(), chunk.size());
      {
        const std::lock_guard lock{state->mutex};
        state->reading = false;
        if (state->abandoned) {
          return;
        }
      }
      if (!result) {
        error = result.error().empty() ? "read failed" : result.error();
        break;
      }
      const auto count = std::min(*result, chunk.size());
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
        } else if (line.truncated) {
          continue;
        } else if (line.text.size() < max_bytes) {
          line.text.push_back(character);
        } else {
          line.truncated = true;
          line.text = std::string{};
        }
      }
    }
  } catch (const std::exception &thrown) {
    error = thrown.what();
  } catch (...) {
    error = "read failed";
  }
  // A partial last line still counts, whether the input ended or failed.
  if (started && !queue(std::move(line))) {
    return;
  }
  const std::lock_guard lock{state->mutex};
  // Cleared here too: a source that threw left it set.
  state->reading = false;
  state->error = std::move(error);
  state->closed = true;
}

LineReader::ReadBytes standard_input_bytes() {
  return [](char *const data,
            const std::size_t size) -> std::expected<std::size_t, std::string> {
    while (true) {
#if defined(_WIN32)
      const auto count = _read(
          0, data, static_cast<unsigned>(std::min<std::size_t>(size, INT_MAX)));
#else
      const auto count = ::read(STDIN_FILENO, data, size);
#endif
      if (count >= 0) {
        return static_cast<std::size_t>(count);
      }
      if (errno != EINTR) {
        return std::unexpected(std::system_category().message(errno));
      }
    }
  };
}

} // namespace pigpen::cli
