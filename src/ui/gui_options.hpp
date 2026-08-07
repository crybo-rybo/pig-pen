#pragma once

#include "agent/config.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace pigpen::ui {

struct GuiOptions {
  agent::Config config{};
  bool help{};
};

[[nodiscard]] std::expected<GuiOptions, std::string>
parse_gui_options(std::span<const std::string_view> arguments);

} // namespace pigpen::ui
