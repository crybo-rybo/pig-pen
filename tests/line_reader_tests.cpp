/// @file line_reader_tests.cpp
/// @brief Covers LineReader with scripted byte sources: lines split across
/// reads of any size, `\r` kept, blank lines numbered, a last line without
/// a terminator, over-long lines truncated and flagged, closing only once
/// every line is taken, the queue capacity holding the source back, a
/// throwing source ending the input, and destruction during a blocked read.

#include "cli/line_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using pigpen::cli::InputLine;
using pigpen::cli::LineReader;
using pigpen::cli::LineStatus;

/// @brief A source that hands out @p text at most @p step bytes per read.
[[nodiscard]] LineReader::ReadBytes text_source(std::string text,
                                                const std::size_t step) {
  auto remaining = std::make_shared<std::string>(std::move(text));
  return [remaining, step](char *const data, const std::size_t size) {
    const auto count = std::min({step, size, remaining->size()});
    std::copy_n(remaining->begin(), count, data);
    remaining->erase(0, count);
    return count;
  };
}

/// @brief Every line up to the close, polling (and briefly sleeping)
/// until the reader says it is closed.
[[nodiscard]] std::vector<InputLine> drain(LineReader &reader) {
  std::vector<InputLine> lines;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    InputLine line;
    const auto status = reader.poll(line);
    if (status == LineStatus::closed) {
      return lines;
    }
    if (status == LineStatus::line) {
      lines.push_back(std::move(line));
    } else {
      std::this_thread::sleep_for(1ms);
    }
  }
  FAIL("the reader never closed");
  return lines;
}

} // namespace

TEST_CASE("lines split across reads of any size come out whole, in order",
          "[lines]") {
  const std::string text = "{\"seed\":1}\n\nsecond\r\n  \nlast";
  const std::vector<InputLine> expected{
      {.number = 1, .text = "{\"seed\":1}"}, {.number = 2, .text = ""},
      {.number = 3, .text = "second\r"},     {.number = 4, .text = "  "},
      {.number = 5, .text = "last"},
  };
  for (const std::size_t step : {1U, 2U, 3U, 7U, 4096U}) {
    LineReader reader{text_source(text, step), 1024, 16};
    CHECK(drain(reader) == expected);
    // Once closed, it stays closed.
    InputLine line;
    CHECK(reader.poll(line) == LineStatus::closed);
  }
}

TEST_CASE("a trailing newline ends the last line, and no input is no lines",
          "[lines]") {
  LineReader ended{text_source("a\nb\n", 3), 1024, 16};
  CHECK(drain(ended) == std::vector<InputLine>{{.number = 1, .text = "a"},
                                               {.number = 2, .text = "b"}});
  LineReader empty{text_source("", 1), 1024, 16};
  CHECK(drain(empty).empty());
}

TEST_CASE("a line past the byte limit keeps its start and is flagged",
          "[lines]") {
  LineReader reader{text_source("abcdefgh\nabcd\nxy", 3), 4, 16};
  CHECK(drain(reader) == std::vector<InputLine>{
                             {.number = 1, .text = "abcd", .truncated = true},
                             {.number = 2, .text = "abcd"},
                             {.number = 3, .text = "xy"},
                         });
}

TEST_CASE("the reader stops reading while its queue is full", "[lines]") {
  // Counts the bytes handed out, one per read.
  auto handed_out = std::make_shared<std::atomic<std::size_t>>(0);
  auto source = text_source("1\n2\n3\n4\n5\n6\n", 1);
  LineReader reader{[source, handed_out](char *data, std::size_t size) {
                      const auto count =
                          source(data, std::min<std::size_t>(size, 1));
                      *handed_out += count;
                      return count;
                    },
                    1024, 2};
  // Two lines fit; the reader then waits before reading line 4.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (handed_out->load() < 6 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  std::this_thread::sleep_for(20ms);
  // Lines 1 and 2 are queued, and line 3 is read and waiting for space.
  CHECK(handed_out->load() == 6);
  InputLine line;
  REQUIRE(reader.poll(line) == LineStatus::line);
  CHECK(line.text == "1");
  const auto lines = drain(reader);
  REQUIRE(lines.size() == 5);
  CHECK(lines.back() == InputLine{.number = 6, .text = "6"});
  CHECK(handed_out->load() == 12);
}

TEST_CASE("a source that throws ends the input after the lines before it",
          "[lines]") {
  auto calls = std::make_shared<int>(0);
  LineReader reader{[calls](char *data, std::size_t) -> std::size_t {
                      if (++*calls == 1) {
                        data[0] = 'a';
                        data[1] = '\n';
                        return 2;
                      }
                      throw std::runtime_error{"gone"};
                    },
                    1024, 16};
  CHECK(drain(reader) == std::vector<InputLine>{{.number = 1, .text = "a"}});
}

TEST_CASE("destroying a reader blocked in a read does not wait for it",
          "[lines]") {
  struct Gate {
    std::mutex mutex{};
    std::condition_variable changed{};
    bool entered{};
    bool open{};
  };
  auto gate = std::make_shared<Gate>();
  {
    LineReader reader{[gate](char *, std::size_t) -> std::size_t {
                        std::unique_lock lock{gate->mutex};
                        gate->entered = true;
                        gate->changed.notify_all();
                        gate->changed.wait(lock, [&] { return gate->open; });
                        return 0;
                      },
                      1024, 16};
    std::unique_lock lock{gate->mutex};
    REQUIRE(gate->changed.wait_for(lock, 10s, [&] { return gate->entered; }));
    InputLine line;
    lock.unlock();
    CHECK(reader.poll(line) == LineStatus::waiting);
    // Leaving the scope detaches the blocked thread instead of joining it.
  }
  // Let the detached thread finish so the test leaves nothing behind.
  {
    const std::lock_guard lock{gate->mutex};
    gate->open = true;
  }
  gate->changed.notify_all();
  std::this_thread::sleep_for(10ms);
}
