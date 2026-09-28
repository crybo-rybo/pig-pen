/// @file termination_signal.hpp
/// @brief SIGINT/SIGTERM as a polled request for cooperative shutdown.
#pragma once

#include <csignal>
#include <expected>
#include <string>

namespace pigpen::cli {

/// @brief Records SIGINT and SIGTERM for a pump loop to poll.
///
/// The handler only stores the signal number in a `volatile sig_atomic_t`,
/// the one async-signal-safe thing it may do; the loop that polls
/// received() starts the cooperative cancellation itself. Handlers are
/// process-wide, so at most one instance should be installed at a time.
class TerminationSignal final {
public:
  TerminationSignal() = default;
  /// @brief Restores the handlers install() replaced.
  ~TerminationSignal();

  TerminationSignal(const TerminationSignal &) = delete;
  TerminationSignal &operator=(const TerminationSignal &) = delete;

  /// @brief Clear any recorded signal and install the handlers.
  /// @return `could not install SIGINT/SIGTERM handlers` on failure, with
  /// nothing left installed.
  [[nodiscard]] std::expected<void, std::string> install();

  /// @brief The most recent SIGINT or SIGTERM number, or 0 when none has
  /// arrived since install().
  [[nodiscard]] int received() const noexcept;

private:
  using Handler = void (*)(int);

  void restore() noexcept;

  Handler previous_interrupt_{SIG_DFL};
  Handler previous_terminate_{SIG_DFL};
  bool installed_{};
};

} // namespace pigpen::cli
