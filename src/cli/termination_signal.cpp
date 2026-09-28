/// @file termination_signal.cpp
/// @brief TerminationSignal implementation; the contract is in the header.
#include "cli/termination_signal.hpp"

namespace pigpen::cli {
namespace {

volatile std::sig_atomic_t received_signal = 0;

extern "C" void record_termination(const int signal_number) noexcept {
  received_signal = signal_number;
}

} // namespace

TerminationSignal::~TerminationSignal() { restore(); }

std::expected<void, std::string> TerminationSignal::install() {
  restore();
  received_signal = 0;
  const auto interrupt = std::signal(SIGINT, record_termination);
  if (interrupt == SIG_ERR) {
    return std::unexpected("could not install SIGINT/SIGTERM handlers");
  }
  const auto terminate = std::signal(SIGTERM, record_termination);
  if (terminate == SIG_ERR) {
    static_cast<void>(std::signal(SIGINT, interrupt));
    return std::unexpected("could not install SIGINT/SIGTERM handlers");
  }
  previous_interrupt_ = interrupt;
  previous_terminate_ = terminate;
  installed_ = true;
  return {};
}

int TerminationSignal::received() const noexcept { return received_signal; }

void TerminationSignal::restore() noexcept {
  if (!installed_) {
    return;
  }
  static_cast<void>(std::signal(SIGINT, previous_interrupt_));
  static_cast<void>(std::signal(SIGTERM, previous_terminate_));
  installed_ = false;
}

} // namespace pigpen::cli
