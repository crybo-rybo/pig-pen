/// @file option_parser.cpp
/// @brief OptionParser implementation; the contract is in the header.
#include "cli/option_parser.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <system_error>

namespace pigpen::cli {
namespace {

/// Help layout: the description starts at this column and wraps before the
/// line would exceed help_width.
constexpr std::size_t help_description_column{28};
constexpr std::size_t help_width{80};

/// @brief Append @p text to @p output, greedily word-wrapped so no line
/// exceeds help_width (unless one word is longer); continuation lines start
/// at the description column.
void append_wrapped(std::string &output, const std::string_view text) {
  const std::string indent(help_description_column, ' ');
  std::size_t column = help_description_column;
  bool line_empty = true;
  std::size_t position = 0;
  while (position < text.size()) {
    const auto start = text.find_first_not_of(' ', position);
    if (start == std::string_view::npos) {
      break;
    }
    // A parenthesised aside, such as "(default: unset)", never splits.
    const auto aside_end =
        text[start] == '(' ? text.find(')', start) : std::string_view::npos;
    const auto end = std::min(
        text.find(' ', aside_end == std::string_view::npos ? start : aside_end),
        text.size());
    const auto word = text.substr(start, end - start);
    if (!line_empty && column + 1 + word.size() > help_width) {
      output += '\n';
      output += indent;
      column = help_description_column;
      line_empty = true;
    }
    if (!line_empty) {
      output += ' ';
      ++column;
    }
    output += word;
    column += word.size();
    line_empty = false;
    position = end;
  }
  output += '\n';
}

} // namespace

std::string format_real(const double number) {
  return number == std::floor(number) ? std::format("{:.1f}", number)
                                      : std::format("{}", number);
}

bool is_valid_utf8(const std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    // The smallest code point each length may encode, so overlong forms fail.
    char32_t code_point = 0;
    char32_t minimum = 0;
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
      code_point = lead & 0x1FU;
      minimum = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
      code_point = lead & 0x0FU;
      minimum = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
      code_point = lead & 0x07U;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (text.size() - index < length) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (next & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      return false;
    }
    index += length;
  }
  return true;
}

ParseResult require_utf8(const std::string_view name,
                         const std::string_view value) {
  if (!is_valid_utf8(value)) {
    return std::unexpected(std::string{name} + " must be valid UTF-8");
  }
  return {};
}

std::expected<std::uint64_t, std::string>
parse_unsigned(const std::string_view name, const std::string_view value,
               const std::uint64_t minimum, const std::uint64_t maximum) {
  // from_chars rejects signs for unsigned targets, so "-1" cannot wrap.
  std::uint64_t number{};
  const auto *const end = value.data() + value.size();
  if (const auto [last, error] = std::from_chars(value.data(), end, number);
      error != std::errc{} || last != end || number < minimum ||
      number > maximum) {
    return std::unexpected(std::string{name} + " must be in the range " +
                           std::to_string(minimum) + ".." +
                           std::to_string(maximum));
  }
  return number;
}

std::expected<double, std::string> parse_real(const std::string_view name,
                                              const std::string_view value,
                                              const double minimum,
                                              const double maximum) {
  double number{};
  const auto *const end = value.data() + value.size();
  if (const auto [last, error] = std::from_chars(value.data(), end, number);
      error != std::errc{} || last != end || !std::isfinite(number) ||
      number < minimum || number > maximum) {
    return std::unexpected(std::string{name} +
                           " must be a finite number in the range " +
                           format_real(minimum) + ".." + format_real(maximum));
  }
  return number;
}

void OptionParser::flag(std::string name, bool &target, const bool setting,
                        std::string description) {
  options_.push_back({
      .name = std::move(name),
      .metavar = {},
      .description = std::move(description),
      .on_value = {},
      .on_flag = [&target, setting] { target = setting; },
  });
}

void OptionParser::real(std::string name, std::string metavar, double &target,
                        const double minimum, const double maximum,
                        std::string description) {
  value(name, std::move(metavar), std::move(description),
        [name, &target, minimum, maximum](const std::string_view parsed) {
          return parse_real(name, parsed, minimum, maximum)
              .transform([&target](const double number) { target = number; });
        });
}

void OptionParser::value(std::string name, std::string metavar,
                         std::string description, ValueHandler handler) {
  options_.push_back({
      .name = std::move(name),
      .metavar = std::move(metavar),
      .description = std::move(description),
      .on_value = std::move(handler),
      .on_flag = {},
  });
}

void OptionParser::rejected(std::string name, std::string message) {
  options_.push_back({
      .name = std::move(name),
      .metavar = {},
      .description = {},
      .on_value = {},
      .on_flag = {},
      .rejection = std::move(message),
  });
}

const OptionParser::Option *
OptionParser::find(const std::string_view name) const {
  const auto found = std::ranges::find(options_, name, &Option::name);
  return found == options_.end() ? nullptr : &*found;
}

ParseResult
OptionParser::parse(const std::span<const std::string_view> arguments) {
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (!argument.starts_with("--")) {
      return std::unexpected("unexpected positional argument: " +
                             std::string{argument});
    }
    const auto equals = argument.find('=');
    const std::string name{argument.substr(0, equals)};
    const auto inline_value = equals == std::string_view::npos
                                  ? std::optional<std::string_view>{}
                                  : argument.substr(equals + 1);

    const auto *const option = find(name);
    if (option == nullptr) {
      return std::unexpected("unknown option: " + name);
    }
    if (option->rejection) {
      return std::unexpected(*option->rejection);
    }
    if (!option->on_value) {
      if (inline_value) {
        return std::unexpected(name + " does not take a value");
      }
      option->on_flag();
      continue;
    }

    std::string_view value;
    if (inline_value && !inline_value->empty()) {
      value = *inline_value;
    } else if (!inline_value && index + 1 < arguments.size() &&
               !arguments[index + 1].starts_with("--")) {
      value = arguments[++index];
    } else {
      return std::unexpected(name + " requires a value");
    }
    if (auto applied = option->on_value(value); !applied) {
      return applied;
    }
  }
  return {};
}

std::string OptionParser::help() const {
  std::string output;
  for (const auto &option : options_) {
    if (option.rejection) {
      continue;
    }
    auto label = "  " + option.name;
    if (!option.metavar.empty()) {
      label += ' ' + option.metavar;
    }
    output += label;
    if (label.size() + 1 > help_description_column) {
      output += '\n';
      output += std::string(help_description_column, ' ');
    } else {
      output += std::string(help_description_column - label.size(), ' ');
    }
    append_wrapped(output, option.description);
  }
  return output;
}

} // namespace pigpen::cli
