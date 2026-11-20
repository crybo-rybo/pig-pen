/// @file world_tool_binding.cpp
/// @brief The world toolbox, its dispatcher, and the dispatcher factory; the
/// contracts are in the headers.
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
WorldToolBinding::move(const WorldToolCall &context,
                       const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.move(arguments.direction);
  auto &activity = record(context, core::ToolKind::move,
                          response.reason ? core::ToolOutcome::blocked_by_wall
                                          : core::ToolOutcome::succeeded,
                          before);
  activity.direction = arguments.direction;
  activity.arguments_json = canonical(arguments);
  return response;
}

core::LookToolResponse
WorldToolBinding::look(const WorldToolCall &context,
                       const DirectionArguments arguments) {
  const auto before = world_.position();
  auto response = tools_.look(arguments.direction);
  auto &activity = record(context, core::ToolKind::look,
                          core::ToolOutcome::succeeded, before);
  activity.direction = arguments.direction;
  activity.arguments_json = canonical(arguments);
  return response;
}

core::EatToolResponse WorldToolBinding::eat(const WorldToolCall &context) {
  const auto before = world_.position();
  auto response = tools_.eat();
  record(context, core::ToolKind::eat,
         response.reason ? core::ToolOutcome::nothing_to_eat
                         : core::ToolOutcome::succeeded,
         before)
      .eaten = response.ate;
  return response;
}

core::ToolActivity &WorldToolBinding::record(const WorldToolCall &context,
                                             const core::ToolKind kind,
                                             const core::ToolOutcome outcome,
                                             const world::Position before) {
  return context.activity.emplace(core::ToolActivity{
      .kind = kind,
      .outcome = outcome,
      .arguments_json = "{}",
      .result_json = "null",
      .before = before,
      .after = world_.position(),
      .score_after = world_.score(),
      .call_id = std::string{context.call.call_id},
      .round = context.call.round,
      .index = context.call.index,
  });
}

WorldToolDispatcher::WorldToolDispatcher(world::World &world,
                                         const core::Config &config,
                                         WorldToolObservers observers)
    : world_(world), observers_(std::move(observers)),
      bound_(scrydantic::bind<WorldToolCall>(
          std::make_shared<WorldToolBinding>(world, config))) {
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
  const bool logging_failed =
      observers_.logging_failed && observers_.logging_failed();
  if (const auto refusal = core::admission_refusal(world_, logging_failed)) {
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

  // The output slot of this call; the member fills it when its action runs.
  std::optional<core::ToolActivity> activity;
  auto result =
      tool->invoke(WorldToolCall{.call = context, .activity = activity},
                   scrydantic::Json{.text = std::string{arguments_json}});
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
  if (observers_.on_activity) {
    observers_.on_activity(std::move(*activity));
  }
  return {.status = core::ToolDispatchStatus::executed,
          .content = std::move(content)};
}

std::unique_ptr<core::IToolDispatcher>
make_world_tool_dispatcher(world::World &world, const core::Config &config,
                           WorldToolObservers observers) {
  return std::make_unique<WorldToolDispatcher>(world, config,
                                               std::move(observers));
}

} // namespace pigpen::agent
