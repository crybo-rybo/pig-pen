/// @file option_parser.hpp
/// @brief The table-driven `--option` parser every front end shares.
///
/// Options are registered against the variables they fill, then one parse()
/// walks the arguments. Both `--option value` and `--option=value` are
/// accepted, and every error message names the option. Nothing here knows
/// about scry, JSON, or a Session.
#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pigpen::cli {

/// @brief Success, or a message naming the option at fault.
using ParseResult = std::expected<void, std::string>;

/// @brief Whether a text option accepts an empty value.
enum class EmptyText : std::uint8_t {
  /// `--name ""` stores an empty string; the caller may reject it later.
  accepted,
  /// An empty value is reported as `NAME requires a value`.
  rejected,
};

/// @brief A number as help and error text write it: at least one decimal
/// place, so 2 reads "2.0" and 0.25 reads "0.25".
[[nodiscard]] std::string format_real(double number);

/// @brief Parse a whole unsigned decimal in [minimum, maximum]; no sign,
/// whitespace, or suffix.
/// @return The number, or `NAME must be in the range MIN..MAX`.
[[nodiscard]] std::expected<std::uint64_t, std::string>
parse_unsigned(std::string_view name, std::string_view value,
               std::uint64_t minimum, std::uint64_t maximum);

/// @brief Parse a finite decimal in [minimum, maximum].
/// @return The number, or `NAME must be a finite number in the range
/// MIN..MAX`, bounds written with at least one decimal place.
[[nodiscard]] std::expected<double, std::string>
parse_real(std::string_view name, std::string_view value, double minimum,
           double maximum);

/// @brief Registered options plus the loop that applies them.
///
/// A value is the rest of the argument after `=` (which must not be empty)
/// or, without `=`, the next argument unless it starts with `--`. A value
/// that is missing is reported as `NAME requires a value`, a flag given one
/// as `NAME does not take a value`, and anything that does not start with
/// `--` as a positional argument. Every argument is parsed even after
/// `--help`, so a bad one is still reported.
class OptionParser final {
public:
  /// @brief Applies one value; returns a message naming the option on
  /// failure.
  using ValueHandler = std::function<ParseResult(std::string_view)>;

  /// @brief `NAME` alone stores @p setting into @p target.
  void flag(std::string name, bool &target, bool setting,
            std::string description);

  /// @brief `NAME VALUE` stores VALUE into @p target, which is any type
  /// assignable from std::string (a string, a path, or an optional of one).
  template <typename Target>
    requires requires(Target &target, std::string parsed) {
      target = std::move(parsed);
    }
  void text(std::string name, std::string metavar, Target &target,
            std::string description, EmptyText empty = EmptyText::accepted) {
    value(name, std::move(metavar), std::move(description),
          [name, &target, empty](const std::string_view parsed) -> ParseResult {
            if (parsed.empty() && empty == EmptyText::rejected) {
              return std::unexpected(name + " requires a value");
            }
            target = std::string{parsed};
            return {};
          });
  }

  /// @brief `NAME VALUE` stores a whole number in [minimum, maximum].
  template <std::unsigned_integral Target>
  void integer(std::string name, std::string metavar, Target &target,
               const std::uint64_t minimum, const std::uint64_t maximum,
               std::string description) {
    value(name, std::move(metavar), std::move(description),
          [name, &target, minimum, maximum](const std::string_view parsed) {
            return parse_unsigned(name, parsed, minimum, maximum)
                .transform([&target](const std::uint64_t number) {
                  target = static_cast<Target>(number);
                });
          });
  }

  /// @brief As integer(), engaging @p target only when the option appears.
  template <std::unsigned_integral Target>
  void integer(std::string name, std::string metavar,
               std::optional<Target> &target, const std::uint64_t minimum,
               const std::uint64_t maximum, std::string description) {
    value(name, std::move(metavar), std::move(description),
          [name, &target, minimum, maximum](const std::string_view parsed) {
            return parse_unsigned(name, parsed, minimum, maximum)
                .transform([&target](const std::uint64_t number) {
                  target = static_cast<Target>(number);
                });
          });
  }

  /// @brief `NAME VALUE` stores a finite number in [minimum, maximum].
  void real(std::string name, std::string metavar, double &target,
            double minimum, double maximum, std::string description);

  /// @brief `NAME VALUE` hands VALUE to @p handler; for anything the other
  /// registrations cannot express, such as a repeatable assignment.
  void value(std::string name, std::string metavar, std::string description,
             ValueHandler handler);

  /// @brief Apply @p arguments (without the program name) in order.
  /// @return The first error; targets set before it keep their values.
  [[nodiscard]] ParseResult parse(std::span<const std::string_view> arguments);

  /// @brief One line per option in registration order, `  NAME METAVAR`
  /// padded to a fixed column, then the description word-wrapped to 80
  /// columns. A parenthesised aside wraps as one word.
  [[nodiscard]] std::string help() const;

private:
  struct Option {
    std::string name;
    std::string metavar;
    std::string description;
    /// Absent for a flag.
    ValueHandler on_value;
    std::function<void()> on_flag;
  };

  [[nodiscard]] const Option *find(std::string_view name) const;

  std::vector<Option> options_{};
};

} // namespace pigpen::cli
