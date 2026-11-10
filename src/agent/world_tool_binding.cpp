/// @file world_tool_binding.cpp
/// @brief The world toolbox and its dispatcher; the contract is in the
/// header.
#include "agent/world_tool_binding.hpp"

#include <algorithm>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief `{"error": <text>}`, encoded by scrydantic so that any text, also
/// a tool name that the model made up, is escaped correctly.
[[nodiscard]] std::string error_result(std::string text) {
  if (auto encoded = scrydantic::encode(ToolErrorResponse{std::move(text)})) {
    return std::move(encoded->text);
  }
  return R"({"error":"the tool call failed"})";
}

/// @brief The canonical argument text of a decoded call.
[[nodiscard]] std::string canonical(const DirectionArguments &arguments) {
  auto encoded = scrydantic::encode(arguments);
  return encoded ? std::move(encoded->text) : std::string{"null"};
}

} // namespace

WorldToolBinding::WorldToolBinding(world::World &world,
                                   const core::Config &config)
    : world_(world), tools_(world, config) {}

// Out-of-line destruction keeps GCC's -O3 speculative devirtualization from
// diagnosing unrelated shared_ptr control blocks in this destructor.
WorldToolBinding::~WorldToolBinding() = default;

core::MoveToolResponse
WorldToolBinding::move(const core::ToolCallContext &context,
                       const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.move(arguments.direction);
  auto &activity = stage(context, core::ToolKind::move,
                         response.reason ? core::ToolOutcome::blocked_by_wall
                                         : core::ToolOutcome::succeeded,
                         before);
  activity.direction = arguments.direction;
  activity.arguments_json = canonical(arguments);
  return response;
}

core::LookToolResponse
WorldToolBinding::look(const core::ToolCallContext &context,
                       const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.look(arguments.direction);
  auto &activity = stage(context, core::ToolKind::look,
                         core::ToolOutcome::succeeded, before);
  activity.direction = arguments.direction;
  activity.arguments_json = canonical(arguments);
  return response;
}

core::EatToolResponse
WorldToolBinding::eat(const core::ToolCallContext &context) {
  const auto before = world_.position();
  auto response = tools_.eat();
  stage(context, core::ToolKind::eat,
        response.reason ? core::ToolOutcome::nothing_to_eat
                        : core::ToolOutcome::succeeded,
        before)
      .eaten = response.ate;
  return response;
}

core::ToolActivity &WorldToolBinding::stage(
    const core::ToolCallContext &context, const core::ToolKind kind,
    const core::ToolOutcome outcome, const world::Position before) {
  return staged_.emplace(core::ToolActivity{
      .kind = kind,
      .outcome = outcome,
      .arguments_json = "{}",
      .result_json = "null",
      .before = before,
      .after = world_.position(),
      .score_after = world_.score(),
      .call_id = std::string{context.call_id},
      .round = context.round,
      .index = context.index,
  });
}

std::optional<std::string_view>
WorldToolBinding::admission_refusal(const bool logging_failed) const {
  if (logging_failed) {
    return "World tools are unavailable because the episode log failed. "
           "Summarize the actions already taken.";
  }
  if (world_.all_positive_items_eaten()) {
    return "All positive-value items have been eaten. Summarize the "
           "completed episode without more tools.";
  }
  return std::nullopt;
}

std::optional<core::ToolActivity> WorldToolBinding::take_staged() {
  return std::exchange(staged_, std::nullopt);
}

WorldToolDispatcher::WorldToolDispatcher(
    std::shared_ptr<WorldToolBinding> binding,
    std::function<bool()> logging_failed)
    : binding_(std::move(binding)), logging_failed_(std::move(logging_failed)),
      bound_(scrydantic::bind<core::ToolCallContext>(binding_)) {
  std::vector<std::string_view> names;
  for (const auto &tool : bound_) {
    tools_.push_back({.name = tool.name,
                      .description = tool.description,
                      .parameters_json_schema = tool.input_schema.text});
    names.push_back(tool.name);
  }
  std::ranges::sort(names);
  for (const auto name : names) {
    registered_names_ += registered_names_.empty() ? "" : ", ";
    registered_names_ += name;
  }
}

WorldToolDispatcher::~WorldToolDispatcher() = default;

core::ToolDispatchResult
WorldToolDispatcher::dispatch(const core::ToolCallContext &context,
                              const std::string_view name,
                              const std::string_view arguments_json) {
  if (const auto refusal =
          binding_->admission_refusal(logging_failed_ && logging_failed_())) {
    return {.status = core::ToolDispatchStatus::refused,
            .content = error_result(std::string{*refusal})};
  }
  const auto tool =
      std::ranges::find(bound_, name, &decltype(bound_)::value_type::name);
  if (tool == bound_.end()) {
    return {.status = core::ToolDispatchStatus::invalid,
            .content =
                error_result("unknown tool \"" + std::string{name} +
                             "\"; registered tools: " + registered_names_)};
  }

  auto result = tool->invoke(
      context, scrydantic::Json{.text = std::string{arguments_json}});
  auto activity = binding_->take_staged();
  std::string content;
  if (result) {
    content = std::move(result->text);
  } else {
    const auto &error = result.error();
    content = error_result(error.model_message.empty() ? error.message
                                                       : error.model_message);
  }
  if (!activity) {
    // The arguments did not decode, so no world action ran.
    return {.status = core::ToolDispatchStatus::invalid,
            .content = std::move(content)};
  }
  activity->result_json = content;
  if (on_activity) {
    on_activity(std::move(*activity));
  }
  return {.status = core::ToolDispatchStatus::executed,
          .content = std::move(content)};
}

} // namespace pigpen::agent
