/// @file prompt_text.hpp
/// @brief The embedded, compile-time decoded model-facing text catalog.
#pragma once

#include "text/catalog.hpp"

namespace pigpen::agent::prompt_text {
namespace detail {
inline constexpr unsigned char source[] = {
#embed "../../resources/prompts.json"
};
inline constexpr auto catalog = text::make_catalog<source>();
} // namespace detail

inline constexpr auto known_item_values =
    detail::catalog.get("known_item_values");
inline constexpr auto hidden_item_values =
    detail::catalog.get("hidden_item_values");
inline constexpr auto opaque_look = detail::catalog.get("opaque_look");
inline constexpr auto transparent_look =
    detail::catalog.get("transparent_look");
inline constexpr auto reward_feedback = detail::catalog.get("reward_feedback");
inline constexpr auto no_reward_feedback =
    detail::catalog.get("no_reward_feedback");
inline constexpr auto system_intro = detail::catalog.get("system_intro");
inline constexpr auto system_coordinates =
    detail::catalog.get("system_coordinates");
inline constexpr auto system_tools = detail::catalog.get("system_tools");
inline constexpr auto system_limits = detail::catalog.get("system_limits");
inline constexpr auto turn_instructions =
    detail::catalog.get("turn_instructions");
inline constexpr auto zero_tool_correction =
    detail::catalog.get("zero_tool_correction");
inline constexpr auto human_guidance = detail::catalog.get("human_guidance");
inline constexpr auto tool_move = detail::catalog.get("tool_move");
inline constexpr auto tool_look = detail::catalog.get("tool_look");
inline constexpr auto tool_eat = detail::catalog.get("tool_eat");
inline constexpr auto tool_direction = detail::catalog.get("tool_direction");
inline constexpr auto budget_exhausted =
    detail::catalog.get("budget_exhausted");
inline constexpr auto budget_one_remaining =
    detail::catalog.get("budget_one_remaining");
inline constexpr auto budget_many_remaining =
    detail::catalog.get("budget_many_remaining");
inline constexpr auto budget_rejected = detail::catalog.get("budget_rejected");

} // namespace pigpen::agent::prompt_text
