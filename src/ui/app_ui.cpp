/// @file app_ui.cpp
/// @brief Panel drawing and widget state for the Dear ImGui front end.
#include "ui/app_ui.hpp"

#include "agent/episode_runner.hpp"
#include "agent/episode_summary.hpp"
#include "agent/reward.hpp"
#include "world/world.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace pigpen::ui {
namespace {

struct ScenarioPreset final {
  const char *name;
  const char *variant;
  bool known_item_values;
  bool reward_feedback;
  bool opaque_look;
};

constexpr std::array scenario_presets{
    ScenarioPreset{"Default", "default", true, true, false},
    ScenarioPreset{"Hidden values", "hidden-values", false, true, false},
    ScenarioPreset{"No reward feedback", "no-reward-feedback", true, false,
                   false},
    ScenarioPreset{"Opaque look", "opaque-look", true, true, true},
    ScenarioPreset{"Blind learning", "blind-learning", false, false, true},
};

[[nodiscard]] std::string_view trim(const std::string_view value) {
  constexpr std::string_view whitespace{" \t\n\v\f\r"};
  const auto first = value.find_first_not_of(whitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

[[nodiscard]] std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](const char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

/// @brief Case-insensitive activity-log filter over tick, turn, tool name,
/// outcome, and the exact argument/result JSON text.
/// @param lowercase_filter Already trimmed and lowercased.
[[nodiscard]] bool matches_filter(const agent::ToolActivity &activity,
                                  const std::string_view lowercase_filter) {
  if (lowercase_filter.empty()) {
    return true;
  }
  auto searchable = std::to_string(activity.tick) + " " +
                    std::to_string(activity.turn) + " " +
                    std::string{agent::tool_kind_name(activity.kind)} + " " +
                    std::string{agent::tool_outcome_name(activity.outcome)} +
                    " " + activity.arguments_json + " " + activity.result_json;
  return lowercase(std::move(searchable)).find(lowercase_filter) !=
         std::string::npos;
}

/// @brief Truncates canonical JSON text with an ellipsis for compact cells.
[[nodiscard]] std::string compact(const std::string_view value,
                                  const std::size_t maximum = 150U) {
  auto text = std::string{value};
  if (text.size() > maximum) {
    text.resize(maximum - 3U);
    text += "...";
  }
  return text;
}

[[nodiscard]] ImU32 item_color(const world::ItemType item) noexcept {
  switch (item) {
  case world::ItemType::berry:
    return IM_COL32(177, 92, 255, 255);
  case world::ItemType::apple:
    return IM_COL32(244, 76, 92, 255);
  case world::ItemType::truffle:
    return IM_COL32(242, 187, 69, 255);
  case world::ItemType::toadstool:
    return IM_COL32(231, 95, 63, 255);
  }
  return IM_COL32(220, 220, 220, 255);
}

/// @brief Draws one item glyph, scaled to the current cell size.
void draw_item(ImDrawList &draw_list, const world::ItemType item,
               const ImVec2 center, const float cell_size) {
  const auto radius = std::max(2.5F, cell_size * 0.13F);
  const auto color = item_color(item);
  switch (item) {
  case world::ItemType::berry:
    draw_list.AddCircleFilled({center.x - radius * 0.55F, center.y},
                              radius * 0.7F, color, 12);
    draw_list.AddCircleFilled({center.x + radius * 0.55F, center.y},
                              radius * 0.7F, color, 12);
    draw_list.AddCircleFilled({center.x, center.y - radius * 0.65F},
                              radius * 0.7F, color, 12);
    break;
  case world::ItemType::apple:
    draw_list.AddCircleFilled(center, radius, color, 18);
    draw_list.AddLine({center.x, center.y - radius},
                      {center.x + radius * 0.25F, center.y - radius * 1.65F},
                      IM_COL32(111, 77, 47, 255), 2.0F);
    draw_list.AddCircleFilled(
        {center.x + radius * 0.55F, center.y - radius * 1.4F}, radius * 0.35F,
        IM_COL32(98, 201, 105, 255), 10);
    break;
  case world::ItemType::truffle:
    draw_list.AddQuadFilled({center.x, center.y - radius * 1.2F},
                            {center.x + radius * 1.15F, center.y},
                            {center.x, center.y + radius * 1.2F},
                            {center.x - radius * 1.15F, center.y}, color);
    draw_list.AddCircle(center, radius * 0.45F, IM_COL32(103, 68, 32, 255), 12,
                        1.5F);
    break;
  case world::ItemType::toadstool:
    draw_list.AddRectFilled(
        {center.x - radius * 0.25F, center.y},
        {center.x + radius * 0.25F, center.y + radius * 1.2F},
        IM_COL32(229, 218, 185, 255), radius * 0.12F);
    draw_list.AddTriangleFilled(
        {center.x - radius * 1.25F, center.y + radius * 0.1F},
        {center.x + radius * 1.25F, center.y + radius * 0.1F},
        {center.x, center.y - radius * 1.0F}, color);
    break;
  }
}

[[nodiscard]] const char *role_name(const agent::TranscriptRole role) noexcept {
  switch (role) {
  case agent::TranscriptRole::automatic:
    return "Automatic instructions";
  case agent::TranscriptRole::guidance:
    return "Human guidance";
  case agent::TranscriptRole::assistant:
    return "Model narration";
  case agent::TranscriptRole::error:
    return "Error";
  }
  return "Unknown";
}

[[nodiscard]] ImVec4 role_color(const agent::TranscriptRole role) noexcept {
  switch (role) {
  case agent::TranscriptRole::automatic:
    return {0.48F, 0.72F, 1.0F, 1.0F};
  case agent::TranscriptRole::guidance:
    return {0.78F, 0.64F, 1.0F, 1.0F};
  case agent::TranscriptRole::assistant:
    return {0.52F, 0.92F, 0.72F, 1.0F};
  case agent::TranscriptRole::error:
    return {1.0F, 0.4F, 0.4F, 1.0F};
  }
  return {1.0F, 1.0F, 1.0F, 1.0F};
}

/// @brief Human-readable transcript label for a decoded call, including the
/// budget or world failure reason when the action changed nothing.
[[nodiscard]] std::string
decoded_call_label(const agent::ToolActivity &activity) {
  auto label = std::string{agent::tool_kind_name(activity.kind)};
  if (activity.direction) {
    label += " " + std::string{world::direction_name(*activity.direction)};
  }
  if (!activity.succeeded()) {
    label +=
        " (failed: " + std::string{agent::tool_outcome_name(activity.outcome)} +
        ")";
  }
  return label;
}

/// @brief Reward total and a collapsible per-term table, part of Stats.
void draw_reward_breakdown(const agent::EpisodeSummary &summary) {
  const auto &reward = summary.reward;
  ImGui::Text("Reward");
  ImGui::SameLine();
  if (reward.valid) {
    ImGui::TextColored(reward.total >= 0.0 ? ImVec4{0.46F, 0.92F, 0.62F, 1.0F}
                                           : ImVec4{1.0F, 0.42F, 0.42F, 1.0F},
                       "%.3f", reward.total);
  } else if (!summary.complete()) {
    ImGui::TextDisabled("%.3f (provisional until the episode ends)",
                        reward.total);
  } else {
    ImGui::TextColored({1.0F, 0.52F, 0.32F, 1.0F}, "invalid (%s)",
                       reward.invalid_reason.c_str());
  }
  if (!ImGui::TreeNode("Reward breakdown")) {
    return;
  }
  if (ImGui::BeginTable("reward-table", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Term");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Contribution");
    ImGui::TableHeadersRow();
    const auto counts = agent::reward_term_counts(reward);
    for (std::size_t index = 0; index < counts.size(); ++index) {
      const auto name = std::string{agent::reward_weight_fields[index].name};
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%g", counts[index]);
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%+.3f", reward.terms.at(name));
    }
    ImGui::EndTable();
  }
  ImGui::TextDisabled("Requests: %u executed, %u invalid, %u over budget, "
                      "%u refused by the host",
                      summary.calls.executed, summary.calls.invalid,
                      summary.calls.budget_refused, summary.calls.host_refused);
  ImGui::TreePop();
}

} // namespace

AppUi::AppUi(const agent::Config &initial_config,
             agent::SessionOptions session_options)
    : session_options_{std::move(session_options)}, controls_{initial_config},
      turn_budget_{static_cast<int>(initial_config.turn_budget)},
      max_tool_rounds_{static_cast<int>(initial_config.max_tool_rounds)},
      use_sampling_seed_{initial_config.sampling_seed.has_value()},
      sampling_seed_{initial_config.sampling_seed.value_or(0U)} {
  if (initial_config.model.empty()) {
    status_message_ =
        "Enter a model identifier in Controls, then press Play or Reset.";
  } else {
    recreate_session(true);
  }
}

void AppUi::pump(const double now_seconds) {
  if (!session_) {
    return;
  }
  pump_stats_ = session_->pump();
  animation_.update(session_->tool_activities(), session_->world().position(),
                    now_seconds);
  if (!session_->metrics_error().empty()) {
    visible_error_ = "Metrics error: " + session_->metrics_error();
  }
}

void AppUi::draw() {
  const auto dockspace = ImGui::DockSpaceOverViewport(
      0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
  // Only lay out a fresh dockspace; a split one was restored from imgui.ini.
  if (!dock_layout_checked_) {
    dock_layout_checked_ = true;
    if (const auto *node = ImGui::DockBuilderGetNode(dockspace);
        node == nullptr || node->IsLeafNode()) {
      build_default_dock_layout(dockspace);
    }
  }

  draw_world_panel();
  draw_transcript_panel();
  draw_event_log_panel();
  draw_controls_panel();
  draw_stats_panel();
  draw_guidance_panel();
}

agent::Config AppUi::config_from_controls() const {
  auto config = controls_;
  config.base_url = trim(controls_.base_url);
  config.model = trim(controls_.model);
  config.turn_budget = static_cast<decltype(config.turn_budget)>(turn_budget_);
  config.max_tool_rounds = static_cast<std::uint32_t>(max_tool_rounds_);
  config.sampling_seed =
      use_sampling_seed_ ? std::optional{sampling_seed_} : std::nullopt;
  return config;
}

void AppUi::recreate_session(const bool auto_play) {
  auto options = session_options_;
  options.prompt_variant =
      preset_ ? scenario_presets[*preset_].variant : "custom";
  auto created =
      agent::Session::create(config_from_controls(), std::move(options));
  if (!created) {
    visible_error_ = "Could not create session: " + created.error();
    return;
  }

  // Destroying the old session writes its footer ("abandoned" if unfinished).
  session_ = std::move(*created);
  animation_.reset(session_->world().position());
  pump_stats_ = {};
  visible_error_.clear();
  status_message_ = "Created session for " + session_->config().model +
                    " (seed " + std::to_string(session_->config().seed) + ").";
  if (auto_play) {
    if (session_->play()) {
      status_message_ += " Auto-play started.";
    } else {
      visible_error_ = "Session was created but could not start playing.";
    }
  }
}

void AppUi::queue_guidance() {
  const auto message = trim(guidance_);
  if (message.empty()) {
    return;
  }
  if (!session_) {
    visible_error_ = "Create a session before queuing guidance.";
    return;
  }
  if (session_->runner().snapshot().state == agent::RunState::finished) {
    visible_error_ = "This episode is finished; Reset before queuing guidance.";
    return;
  }
  static_cast<void>(session_->queue_user_input(std::string{message}));
  guidance_.clear();
  status_message_ = "Guidance added to the FIFO queue.";
  visible_error_.clear();
}

void AppUi::build_default_dock_layout(const std::uint32_t dockspace_id) {
  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id,
                                ImGui::GetMainViewport()->WorkSize);

  ImGuiID guidance{};
  ImGuiID upper{};
  ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.15F, &guidance,
                              &upper);

  ImGuiID right{};
  ImGuiID world{};
  ImGui::DockBuilderSplitNode(upper, ImGuiDir_Right, 0.40F, &right, &world);

  ImGuiID controls{};
  ImGuiID right_remainder{};
  ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.29F, &controls,
                              &right_remainder);

  ImGuiID stats{};
  ImGuiID transcript_and_events{};
  ImGui::DockBuilderSplitNode(right_remainder, ImGuiDir_Down, 0.22F, &stats,
                              &transcript_and_events);

  ImGuiID event_log{};
  ImGuiID transcript{};
  ImGui::DockBuilderSplitNode(transcript_and_events, ImGuiDir_Down, 0.43F,
                              &event_log, &transcript);

  ImGui::DockBuilderDockWindow("World", world);
  ImGui::DockBuilderDockWindow("Transcript", transcript);
  ImGui::DockBuilderDockWindow("Event Log", event_log);
  ImGui::DockBuilderDockWindow("Controls", controls);
  ImGui::DockBuilderDockWindow("Stats", stats);
  ImGui::DockBuilderDockWindow("Guidance", guidance);
  ImGui::DockBuilderFinish(dockspace_id);
}

void AppUi::draw_world_panel() {
  if (!ImGui::Begin("World")) {
    ImGui::End();
    return;
  }
  if (!session_) {
    ImGui::TextDisabled(
        "No active session. Check Controls for startup errors.");
    ImGui::End();
    return;
  }

  const auto &simulation = session_->world();
  const auto available = ImGui::GetContentRegionAvail();
  const auto grid_size =
      std::max(120.0F, std::min(available.x - 8.0F, available.y - 104.0F));
  const auto cell_size = grid_size / static_cast<float>(world::World::width);
  const auto cursor = ImGui::GetCursorScreenPos();
  const ImVec2 grid_min{
      cursor.x + std::max(0.0F, (available.x - grid_size) * 0.5F), cursor.y};
  const ImVec2 grid_max{grid_min.x + grid_size, grid_min.y + grid_size};

  ImGui::SetCursorScreenPos(grid_min);
  ImGui::InvisibleButton("world-grid", {grid_size, grid_size});
  auto *draw_list = ImGui::GetWindowDrawList();

  const auto cell_center = [grid_min, cell_size](const float x, const float y) {
    return ImVec2{
        grid_min.x + (x + 0.5F) * cell_size,
        grid_min.y +
            (static_cast<float>(world::World::height) - y - 0.5F) * cell_size,
    };
  };

  for (int y = 0; y < world::World::height; ++y) {
    for (int x = 0; x < world::World::width; ++x) {
      const world::Position position{.x = x, .y = y};
      const ImVec2 top_left{
          grid_min.x + static_cast<float>(x) * cell_size,
          grid_min.y +
              static_cast<float>(world::World::height - 1 - y) * cell_size,
      };
      const ImVec2 bottom_right{top_left.x + cell_size, top_left.y + cell_size};
      const auto checker = ((x + y) % 2) == 0;
      draw_list->AddRectFilled(top_left, bottom_right,
                               checker ? IM_COL32(45, 53, 68, 255)
                                       : IM_COL32(40, 48, 62, 255));
      if (!simulation.is_observed(position)) {
        draw_list->AddRectFilled(top_left, bottom_right,
                                 IM_COL32(8, 12, 20, 150));
      } else {
        draw_list->AddRect(top_left, bottom_right, IM_COL32(66, 164, 150, 95),
                           0.0F, 0, 1.5F);
      }
      if (const auto item = simulation.item_at(position)) {
        draw_item(
            *draw_list, *item,
            {top_left.x + cell_size * 0.5F, top_left.y + cell_size * 0.5F},
            cell_size);
      }
      draw_list->AddRect(top_left, bottom_right, IM_COL32(93, 105, 124, 170));
    }
  }

  if (const auto effect = animation_.active_effect()) {
    const auto pulse = std::sin(effect->progress * std::numbers::pi_v<float>);
    const auto alpha = static_cast<int>(80.0F + pulse * 175.0F);
    const auto origin = cell_center(static_cast<float>(effect->origin.x),
                                    static_cast<float>(effect->origin.y));
    if (effect->kind == agent::ToolKind::look && effect->direction) {
      auto endpoint = origin;
      switch (*effect->direction) {
      case world::Direction::north:
        endpoint.y = grid_min.y;
        break;
      case world::Direction::south:
        endpoint.y = grid_max.y;
        break;
      case world::Direction::east:
        endpoint.x = grid_max.x;
        break;
      case world::Direction::west:
        endpoint.x = grid_min.x;
        break;
      }
      draw_list->AddLine(origin, endpoint, IM_COL32(94, 225, 255, alpha),
                         std::max(2.0F, cell_size * 0.08F));
      draw_list->AddCircle(origin, cell_size * (0.25F + 0.12F * pulse),
                           IM_COL32(94, 225, 255, alpha), 24, 2.0F);
    } else if (effect->kind == agent::ToolKind::eat) {
      draw_list->AddCircle(
          origin, cell_size * (0.2F + effect->progress * 0.48F),
          IM_COL32(255, 215, 92, alpha), 32, std::max(2.0F, cell_size * 0.07F));
    }
  }

  const auto animated = animation_.blob_position();
  const auto blob_center = cell_center(animated.x, animated.y);
  const auto blob_radius = std::max(5.0F, cell_size * 0.22F);
  draw_list->AddCircleFilled(blob_center, blob_radius,
                             IM_COL32(73, 223, 185, 255), 32);
  draw_list->AddCircle(blob_center, blob_radius, IM_COL32(236, 255, 250, 255),
                       32, 2.0F);
  draw_list->AddCircleFilled({blob_center.x - blob_radius * 0.32F,
                              blob_center.y - blob_radius * 0.15F},
                             std::max(1.0F, blob_radius * 0.10F),
                             IM_COL32(18, 48, 49, 255), 10);
  draw_list->AddCircleFilled({blob_center.x + blob_radius * 0.32F,
                              blob_center.y - blob_radius * 0.15F},
                             std::max(1.0F, blob_radius * 0.10F),
                             IM_COL32(18, 48, 49, 255), 10);

  if (const auto item = simulation.item_at(simulation.position())) {
    const auto actual_center =
        cell_center(static_cast<float>(simulation.position().x),
                    static_cast<float>(simulation.position().y));
    draw_list->AddCircle(actual_center, blob_radius + 4.0F, item_color(*item),
                         32, 3.0F);
    draw_item(*draw_list, *item,
              {actual_center.x + blob_radius * 1.15F,
               actual_center.y - blob_radius * 1.15F},
              cell_size * 0.62F);
  }

  if (ImGui::IsItemHovered()) {
    const auto mouse = ImGui::GetMousePos();
    const auto grid_x = static_cast<int>((mouse.x - grid_min.x) / cell_size);
    const auto screen_y = static_cast<int>((mouse.y - grid_min.y) / cell_size);
    const auto grid_y = world::World::height - 1 - screen_y;
    const world::Position hovered{.x = grid_x, .y = grid_y};
    if (world::World::in_bounds(hovered)) {
      ImGui::BeginTooltip();
      ImGui::Text("Cell (%d, %d)", grid_x, grid_y);
      ImGui::Text("Observed by model: %s",
                  simulation.is_observed(hovered) ? "yes" : "no");
      if (const auto item = simulation.item_at(hovered)) {
        ImGui::Text("Item: %s", world::item_name(*item).data());
      } else {
        ImGui::TextDisabled("No item");
      }
      ImGui::EndTooltip();
    }
  }

  ImGui::SetCursorScreenPos({grid_min.x, grid_max.y + 7.0F});
  ImGui::TextDisabled(
      "Origin (0,0) is south-west. Dark tint = not observed by model.");
  ImGui::Text("Score %d  |  Blob (%d,%d)  |  Visual queue %zu",
              simulation.score(), simulation.position().x,
              simulation.position().y, animation_.queued_action_count());
  if (session_->tool_activities().empty()) {
    ImGui::TextDisabled("Last decoded call: none");
  } else {
    const auto last_call =
        decoded_call_label(session_->tool_activities().back());
    ImGui::TextColored({0.95F, 0.78F, 0.32F, 1.0F}, "Last decoded call: %s",
                       last_call.c_str());
  }
  if (const auto item = simulation.item_at(simulation.position())) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(item_color(*item)),
                       "Standing on: %s (%+d)", world::item_name(*item).data(),
                       world::item_reward(*item));
  } else {
    ImGui::TextDisabled("Standing on: empty cell");
  }
  ImGui::End();
}

void AppUi::draw_transcript_panel() {
  if (!ImGui::Begin("Transcript")) {
    ImGui::End();
    return;
  }
  ImGui::Checkbox("Auto-scroll", &transcript_auto_scroll_);
  ImGui::SameLine();
  ImGui::TextDisabled(
      "Decoded world-tool calls are sourced from typed activity");
  ImGui::Separator();

  if (!session_) {
    ImGui::TextDisabled("No transcript yet.");
    ImGui::End();
    return;
  }

  const auto &transcript = session_->runner().transcript();
  const auto &activities = session_->tool_activities();
  const auto snapshot = session_->runner().snapshot();

  ImGui::BeginChild("transcript-scroll", {0.0F, 0.0F}, ImGuiChildFlags_Borders);
  if (transcript.empty()) {
    ImGui::TextDisabled("The first automatic turn will appear here.");
  }
  for (const auto &entry : transcript) {
    if (entry.role == agent::TranscriptRole::assistant) {
      ImGui::TextColored({0.95F, 0.78F, 0.32F, 1.0F},
                         "Turn %u · Decoded world-tool calls", entry.turn);
      // The activity feed is appended in turn order.
      const auto calls = std::ranges::equal_range(
          activities, std::size_t{entry.turn}, {}, &agent::ToolActivity::turn);
      for (const auto &activity : calls) {
        const auto arguments = compact(activity.arguments_json, 72U);
        const auto result = compact(activity.result_json, 110U);
        ImGui::TextColored({0.95F, 0.78F, 0.32F, 1.0F}, "  %s",
                           agent::tool_kind_name(activity.kind).data());
        ImGui::SameLine();
        ImGui::TextDisabled("%s -> %s", arguments.c_str(), result.c_str());
      }
      if (calls.empty()) {
        if (snapshot.turn_in_flight && entry.turn == snapshot.turns_used + 1U) {
          ImGui::TextDisabled("  No decoded world-tool calls yet");
        } else {
          ImGui::TextColored({1.0F, 0.52F, 0.32F, 1.0F},
                             "  No decoded world-tool calls");
        }
      }
      ImGui::TextColored(role_color(entry.role), "%s", role_name(entry.role));
      ImGui::PushTextWrapPos(0.0F);
      if (entry.text.empty()) {
        ImGui::TextDisabled("Waiting for model output...");
      } else {
        ImGui::TextUnformatted(entry.text.c_str());
      }
      ImGui::PopTextWrapPos();
    } else {
      ImGui::TextColored(role_color(entry.role), "Turn %u · %s", entry.turn,
                         role_name(entry.role));
      ImGui::PushTextWrapPos(0.0F);
      ImGui::TextUnformatted(entry.text.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
  }
  // Follow new output only while the view is already at the bottom, so
  // scrolling up to read history is not yanked back every frame.
  if (transcript_auto_scroll_ &&
      ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
    ImGui::SetScrollHereY(1.0F);
  }
  ImGui::EndChild();
  ImGui::End();
}

void AppUi::draw_event_log_panel() {
  if (!ImGui::Begin("Event Log")) {
    ImGui::End();
    return;
  }
  ImGui::SetNextItemWidth(
      std::max(120.0F, ImGui::GetContentRegionAvail().x - 115.0F));
  ImGui::InputTextWithHint("##event-filter", "Filter tool, args, result...",
                           &event_filter_);
  ImGui::SameLine();
  ImGui::TextDisabled("%zu decoded calls",
                      session_ ? session_->tool_activities().size() : 0U);

  if (!session_) {
    ImGui::TextDisabled("No tool events yet.");
    ImGui::End();
    return;
  }

  constexpr auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                         ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("events", 5, flags, {0.0F, 0.0F})) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 45.0F);
    ImGui::TableSetupColumn("Turn", ImGuiTableColumnFlags_WidthFixed, 45.0F);
    ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, 65.0F);
    ImGui::TableSetupColumn("Arguments");
    ImGui::TableSetupColumn("Result");
    ImGui::TableHeadersRow();
    const auto filter = lowercase(std::string{trim(event_filter_)});
    for (const auto &activity : session_->tool_activities()) {
      if (!matches_filter(activity, filter)) {
        continue;
      }
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%llu", static_cast<unsigned long long>(activity.tick));
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%zu", activity.turn);
      ImGui::TableSetColumnIndex(2);
      ImGui::TextColored({0.85F, 0.72F, 0.35F, 1.0F}, "%s",
                         agent::tool_kind_name(activity.kind).data());

      const auto arguments_short = compact(activity.arguments_json);
      const auto result_short = compact(activity.result_json);
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(arguments_short.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", activity.arguments_json.c_str());
      }
      ImGui::TableSetColumnIndex(4);
      if (!activity.succeeded()) {
        ImGui::TextColored({1.0F, 0.58F, 0.35F, 1.0F}, "%s",
                           result_short.c_str());
      } else {
        ImGui::TextUnformatted(result_short.c_str());
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", activity.result_json.c_str());
      }
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

void AppUi::draw_controls_panel() {
  if (!ImGui::Begin("Controls")) {
    ImGui::End();
    return;
  }

  ImGui::SeparatorText("Connection");
  ImGui::InputText("Base URL", &controls_.base_url);
  ImGui::InputText("Model (required)", &controls_.model);

  ImGui::SeparatorText("Scenario");
  if (ImGui::BeginCombo("Preset",
                        preset_ ? scenario_presets[*preset_].name : "Custom")) {
    for (std::size_t index = 0; index < scenario_presets.size(); ++index) {
      const auto &preset = scenario_presets[index];
      if (ImGui::Selectable(preset.name, preset_ == index)) {
        preset_ = index;
        controls_.known_item_values = preset.known_item_values;
        controls_.reward_feedback = preset.reward_feedback;
        controls_.opaque_look = preset.opaque_look;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::Checkbox("Known item values", &controls_.known_item_values)) {
    preset_.reset();
  }
  if (ImGui::Checkbox("Reward feedback", &controls_.reward_feedback)) {
    preset_.reset();
  }
  if (ImGui::Checkbox("Opaque look", &controls_.opaque_look)) {
    preset_.reset();
  }

  ImGui::InputScalar("Seed", ImGuiDataType_U64, &controls_.seed);
  ImGui::SameLine();
  if (ImGui::Button("Reroll + Reset")) {
    std::random_device entropy;
    controls_.seed = (std::uint64_t{entropy()} << 32U) | entropy();
    recreate_session(true);
  }
  // Clamp to the ranges Session::create accepts as soon as they are edited.
  ImGui::InputInt("Turn budget", &turn_budget_);
  turn_budget_ =
      std::clamp(turn_budget_, 1, static_cast<int>(agent::turn_budget_limit));
  ImGui::InputInt("Tool rounds / turn", &max_tool_rounds_);
  max_tool_rounds_ = std::clamp(max_tool_rounds_, 1,
                                static_cast<int>(agent::tool_rounds_limit));
  ImGui::InputDouble("Temperature", &controls_.temperature, 0.1, 0.5, "%.2f");
  controls_.temperature = std::clamp(controls_.temperature, 0.0, 2.0);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Sampling control from 0.0 to 2.0; applies on Reset.");
  }
  ImGui::Checkbox("##use_sampling_seed", &use_sampling_seed_);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Send a provider sampling seed; applies on Reset.");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!use_sampling_seed_);
  ImGui::InputScalar("Sampling seed", ImGuiDataType_U32, &sampling_seed_);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Best-effort repeatability on OpenAI-compatible servers; "
                      "independent of the world seed. Applies on Reset.");
  }
  if (ImGui::SliderFloat("Animation speed", &animation_speed_, 0.25F, 4.0F,
                         "%.2fx")) {
    animation_.set_speed(animation_speed_);
  }

  ImGui::SeparatorText("Episode");
  const auto snapshot =
      session_ ? session_->runner().snapshot() : agent::EpisodeSnapshot{};
  const auto can_play = !session_ || snapshot.state == agent::RunState::idle ||
                        snapshot.state == agent::RunState::paused;
  ImGui::BeginDisabled(!can_play);
  if (ImGui::Button(snapshot.state == agent::RunState::paused ? "Resume"
                                                              : "Play")) {
    if (session_) {
      static_cast<void>(session_->play());
    } else {
      recreate_session(true);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!session_ || snapshot.state != agent::RunState::playing);
  if (ImGui::Button("Pause")) {
    static_cast<void>(session_->pause());
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!session_ ||
                       snapshot.state == agent::RunState::finished);
  if (ImGui::Button("Stop")) {
    static_cast<void>(session_->stop());
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    recreate_session(true);
  }

  if (session_) {
    ImGui::Text("State: %s", agent::run_state_name(snapshot.state).data());
    if (config_from_controls() != session_->config()) {
      ImGui::TextColored({1.0F, 0.78F, 0.28F, 1.0F},
                         "Pending settings apply on Reset.");
    }
    const auto &metrics_path = session_->metrics_path();
    ImGui::TextWrapped("Metrics: %s",
                       metrics_path ? metrics_path->string().c_str() : "off");
  }
  if (!status_message_.empty()) {
    ImGui::TextColored({0.48F, 0.88F, 0.68F, 1.0F}, "%s",
                       status_message_.c_str());
  }
  if (!visible_error_.empty()) {
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextColored({1.0F, 0.38F, 0.38F, 1.0F}, "%s",
                       visible_error_.c_str());
    ImGui::PopTextWrapPos();
  }
  if (session_ && !snapshot.error.empty()) {
    ImGui::TextColored({1.0F, 0.38F, 0.38F, 1.0F}, "Episode error: %s",
                       snapshot.error.c_str());
  }
  ImGui::End();
}

void AppUi::draw_stats_panel() {
  if (!ImGui::Begin("Stats")) {
    ImGui::End();
    return;
  }
  if (!session_) {
    ImGui::TextDisabled("No active session.");
    ImGui::End();
    return;
  }

  const auto snapshot = session_->runner().snapshot();
  const auto &simulation = session_->world();
  ImGui::Text("Score");
  ImGui::SameLine();
  ImGui::TextColored(simulation.score() >= 0 ? ImVec4{0.46F, 0.92F, 0.62F, 1.0F}
                                             : ImVec4{1.0F, 0.42F, 0.42F, 1.0F},
                     "%d", simulation.score());
  ImGui::SameLine();
  ImGui::TextDisabled(
      "| Turns %u / %u | Last %lld ms", snapshot.turns_used,
      snapshot.turn_budget,
      static_cast<long long>(snapshot.last_turn_latency.count()));

  if (snapshot.finish_reason) {
    ImGui::Text("Finished: %s",
                agent::finish_reason_name(*snapshot.finish_reason).data());
  } else {
    ImGui::Text("State: %s%s", agent::run_state_name(snapshot.state).data(),
                snapshot.turn_in_flight ? " (model turn active)" : "");
  }

  const auto &activities = session_->tool_activities();
  const auto calls_to = [&activities](const agent::ToolKind kind) {
    return static_cast<std::size_t>(
        std::ranges::count(activities, kind, &agent::ToolActivity::kind));
  };
  const auto eat_attempts = calls_to(agent::ToolKind::eat);
  const auto successful_eats = static_cast<std::size_t>(
      std::ranges::count_if(activities, [](const auto &activity) {
        return activity.eaten.has_value();
      }));

  if (ImGui::BeginTable("stats-table", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Item / tool");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Item / tool");
    ImGui::TableSetupColumn("Count");
    ImGui::TableHeadersRow();
    constexpr std::array item_types{
        world::ItemType::berry,
        world::ItemType::apple,
        world::ItemType::truffle,
        world::ItemType::toadstool,
    };
    const std::array<std::pair<const char *, std::size_t>, 4> tools{
        std::pair{"move", calls_to(agent::ToolKind::move)},
        std::pair{"look", calls_to(agent::ToolKind::look)},
        std::pair{"eat attempts", eat_attempts},
        std::pair{"decoded calls", activities.size()},
    };
    for (std::size_t index = 0; index < item_types.size(); ++index) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(
          ImGui::ColorConvertU32ToFloat4(item_color(item_types[index])),
          "%s eaten", world::item_name(item_types[index]).data());
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%zu", simulation.eaten_count(item_types[index]));
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(tools[index].first);
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%zu", tools[index].second);
    }
    ImGui::EndTable();
  }

  ImGui::TextColored({0.46F, 0.92F, 0.62F, 1.0F}, "Successful eats %zu",
                     successful_eats);
  ImGui::SameLine();
  ImGui::TextColored({1.0F, 0.52F, 0.32F, 1.0F}, "| failed %zu",
                     eat_attempts - successful_eats);

  draw_reward_breakdown(
      agent::summarize_episode(*session_, session_->reward_weights()));

  ImGui::TextDisabled(
      "Callbacks %zu | transport queued %zu | visuals queued %zu",
      pump_stats_.callbacks_delivered, pump_stats_.events_remaining,
      animation_.queued_action_count());
  ImGui::End();
}

void AppUi::draw_guidance_panel() {
  if (!ImGui::Begin("Guidance")) {
    ImGui::End();
    return;
  }
  ImGui::TextDisabled("Optional guidance is delivered one message per turn in "
                      "FIFO order.");
  const auto button_width = 78.0F;
  ImGui::SetNextItemWidth(std::max(120.0F, ImGui::GetContentRegionAvail().x -
                                               button_width - 10.0F));
  const auto submitted = ImGui::InputTextWithHint(
      "##guidance", "e.g. Search the western edge before eating.", &guidance_,
      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const auto clicked = ImGui::Button("Queue", {button_width, 0.0F});
  if (submitted || clicked) {
    queue_guidance();
  }
  if (session_) {
    if (std::ranges::any_of(session_->runner().guidance(),
                            [](const agent::GuidanceEntry &entry) {
                              return entry.status ==
                                     agent::GuidanceStatus::pending;
                            })) {
      if (ImGui::Button("Clear pending")) {
        session_->clear_pending_user_inputs();
        status_message_ = "Cleared pending guidance.";
      }
      ImGui::Separator();
    }

    std::optional<std::uint64_t> remove_id;
    for (const auto &entry : session_->runner().guidance()) {
      ImGui::PushID(static_cast<int>(entry.id));
      if (entry.status == agent::GuidanceStatus::pending) {
        ImGui::TextColored({1.0F, 0.78F, 0.28F, 1.0F},
                           "Pending for turn %u: %s", entry.turn,
                           entry.text.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
          remove_id = entry.id;
        }
      } else {
        ImGui::TextColored({0.48F, 0.88F, 0.68F, 1.0F}, "Sent on turn %u: %s",
                           entry.turn, entry.text.c_str());
      }
      ImGui::PopID();
    }
    if (remove_id && session_->remove_pending_user_input(*remove_id)) {
      status_message_ = "Removed pending guidance.";
    }
  }
  ImGui::End();
}

} // namespace pigpen::ui
