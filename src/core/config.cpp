/// @file config.cpp
/// @brief The llamad default socket path, and the context estimate.
#include "core/config.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace pigpen::core {
namespace {

/// About 860 measured tokens before the first reply (system prompt, tool
/// schemas, first user message), rounded up.
constexpr std::uint64_t context_tokens_before_first_reply{1'000};
/// Each request adds 70 to 100 measured tokens. A turn that spends its four
/// calls in four rounds makes five requests (350 to 500 tokens); a turn that
/// batches its calls makes fewer.
constexpr std::uint64_t context_tokens_per_turn{400};

} // namespace

std::uint64_t
estimated_context_tokens(const std::uint32_t turn_budget) noexcept {
  return context_tokens_before_first_reply +
         context_tokens_per_turn * std::uint64_t{turn_budget};
}

std::string default_socket_path() {
  // Same rule as llamad's own --socket default.
  if (const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      runtime != nullptr && runtime[0] != '\0') {
    return std::string{runtime} + "/llamad.sock";
  }
  return "/tmp/llamad-" + std::to_string(static_cast<unsigned>(::getuid())) +
         ".sock";
}

} // namespace pigpen::core
