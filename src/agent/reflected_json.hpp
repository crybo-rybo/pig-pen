#pragma once

#include "agent/tool_contract.hpp"

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <meta>
#include <nlohmann/json.hpp>
#include <optional>
#include <scry/reflection.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace pigpen::agent {
namespace detail {

template <typename Type> struct OptionalTraits {
  static constexpr bool recognized = false;
};

template <typename Value> struct OptionalTraits<std::optional<Value>> {
  static constexpr bool recognized = true;
  using value_type = Value;
};

template <typename Type> struct SequenceTraits {
  static constexpr bool recognized = false;
};

template <typename Value, typename Allocator>
struct SequenceTraits<std::vector<Value, Allocator>> {
  static constexpr bool recognized = true;
};

template <typename Value, std::size_t Size>
struct SequenceTraits<std::array<Value, Size>> {
  static constexpr bool recognized = true;
};

template <typename Enum>
  requires std::is_enum_v<Enum>
[[nodiscard]] nlohmann::json reflected_enum_json(const Enum value) {
  auto output = nlohmann::json{};
  bool found = false;
  static constexpr auto enumerators =
      std::define_static_array(std::meta::enumerators_of(^^Enum));
  template for (constexpr std::meta::info enumerator : enumerators) {
    constexpr auto candidate =
        std::meta::extract<Enum>(std::meta::constant_of(enumerator));
    if (value == candidate) {
      output = std::string{std::meta::identifier_of(enumerator)};
      found = true;
    }
  }
  if (!found) {
    throw std::logic_error{"reflected enum contains an unnamed value"};
  }
  return output;
}

template <typename Type>
  requires scry::reflection::SupportedValue<std::remove_cvref_t<Type>>
[[nodiscard]] nlohmann::json reflected_json_impl(const Type &value) {
  using Value = std::remove_cvref_t<Type>;
  if constexpr (std::same_as<Value, bool> || std::integral<Value> ||
                std::same_as<Value, std::string>) {
    return value;
  } else if constexpr (std::floating_point<Value>) {
    if (!std::isfinite(value)) {
      throw std::logic_error{"reflected floating-point value must be finite"};
    }
    return value;
  } else if constexpr (std::is_enum_v<Value>) {
    return reflected_enum_json(value);
  } else if constexpr (OptionalTraits<Value>::recognized) {
    if (!value) {
      return nullptr;
    }
    return reflected_json_impl(*value);
  } else if constexpr (SequenceTraits<Value>::recognized) {
    auto output = nlohmann::json::array();
    for (const auto &element : value) {
      output.push_back(reflected_json_impl(element));
    }
    return output;
  } else {
    auto output = nlohmann::json::object();
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^Value, std::meta::access_context::unchecked()));
    template for (constexpr std::meta::info member : members) {
      output[std::string{std::meta::identifier_of(member)}] =
          reflected_json_impl(value.[:member:]);
    }
    return output;
  }
}

} // namespace detail

template <typename Type>
concept ReflectedToolObservation =
    std::same_as<std::remove_cvref_t<Type>, DirectionArguments> ||
    std::same_as<std::remove_cvref_t<Type>, EatArguments> ||
    std::same_as<std::remove_cvref_t<Type>, MoveToolResponse> ||
    std::same_as<std::remove_cvref_t<Type>, LookToolResponse> ||
    std::same_as<std::remove_cvref_t<Type>, EatToolResponse>;

/// Projects a Scry-supported reflected value into nlohmann JSON for Pig Pen's
/// event feed and JSONL diagnostics. Model-bound encoding remains owned by
/// Scry; this adapter is deliberately limited to Pig Pen's current tool
/// contracts and is checked against the real provider path by integration
/// tests.
template <ReflectedToolObservation Type>
[[nodiscard]] nlohmann::json reflected_json(const Type &value) {
  return detail::reflected_json_impl(value);
}

} // namespace pigpen::agent
