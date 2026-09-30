/// @file line_reader.hpp
/// @brief Reads lines on a thread of its own, so a pump loop can take them
/// as they arrive without ever blocking on input.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pigpen::cli {

/// @brief One line as read, without its `\n`.
struct InputLine {
  /// One-based, counting every line read, blank ones included.
  std::size_t number{};
  /// The line's bytes; empty when it was too long.
  std::string text{};
  /// The line was longer than the reader's max_bytes and its bytes were
  /// discarded, so a queue of long lines costs no more than short ones.
  bool truncated{};

  friend bool operator==(const InputLine &, const InputLine &) = default;
};

/// @brief What LineReader::poll() found.
enum class LineStatus : std::uint8_t {
  /// A line was taken.
  line,
  /// Nothing yet; the input is still open.
  waiting,
  /// The input has ended, or failed (see LineReader::error()), and every
  /// line was taken.
  closed,
};

/// @brief A background thread that splits a byte source into lines and
/// queues them for poll(), which never blocks.
///
/// Lines end at `\n`; a `\r` before it is kept, and a last line without a
/// terminator still counts. The reader stops reading while @p capacity lines
/// wait to be taken, so a fast writer is held back by the pipe instead of
/// filling memory.
class LineReader final {
public:
  /// Reads up to `size` bytes into `data`, blocking until at least one is
  /// available; returns how many, 0 at the end of input, or why the input
  /// could not be read.
  using ReadBytes = std::function<std::expected<std::size_t, std::string>(
      char *data, std::size_t size)>;

  /// @param read Called only on the reader's thread, until it returns 0 or
  /// an error, or throws (an error with the exception's message).
  /// @param max_bytes Longest line kept whole; at least 1.
  /// @param capacity Most lines queued at once; at least 1.
  LineReader(ReadBytes read, std::size_t max_bytes, std::size_t capacity);

  /// @brief Joins the thread unless it is inside @ref ReadBytes: a read
  /// nothing can interrupt, such as a pipe whose writer is still open. Then
  /// the thread is detached, reads nothing after that call returns, and is
  /// left to the process's exit.
  ~LineReader();

  LineReader(const LineReader &) = delete;
  LineReader &operator=(const LineReader &) = delete;

  /// @brief Take the next line into @p line if there is one.
  [[nodiscard]] LineStatus poll(InputLine &line);

  /// @brief Why reading stopped early, once poll() has said
  /// LineStatus::closed; empty when the input simply ended. A partial last
  /// line read before an error is still delivered.
  [[nodiscard]] std::string error() const;

private:
  /// Shared with the thread, which may outlive the reader.
  struct State {
    mutable std::mutex mutex{};
    std::condition_variable space{};
    std::deque<InputLine> lines{};
    bool closed{};
    /// Why reading failed, or empty.
    std::string error{};
    /// The thread is inside ReadBytes.
    bool reading{};
    bool abandoned{};
  };

  static void run(const ReadBytes &read, std::size_t max_bytes,
                  std::size_t capacity, const std::shared_ptr<State> &state);

  std::shared_ptr<State> state_;
  std::thread thread_;
};

/// @brief Reads standard input with the operating system's read call, not
/// through C stdio or iostreams, so a read left blocked when the process
/// exits holds no lock that exit needs. An interrupted read is retried; any
/// other failure (a closed descriptor, a directory, an I/O error) is an
/// error naming the system's reason.
[[nodiscard]] LineReader::ReadBytes standard_input_bytes();

} // namespace pigpen::cli
