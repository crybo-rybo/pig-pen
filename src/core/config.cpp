/// @file config.cpp
/// @brief The llamad default socket path.
#include "core/config.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace pigpen::core {

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
