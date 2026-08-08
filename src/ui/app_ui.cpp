#include "ui/app_ui.hpp"

#include "agent/episode_runner.hpp"
#include "world/world.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace pigpen::ui {
namespace {

constexpr float pi = 3.14159265358979323846F;

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
constexpr int custom_preset = static_cast<int>(scenario_presets.size());
constexpr std::array preset_names{
    scenario_presets[0].name, scenario_presets[1].name,
    scenario_presets[2].name, scenario_presets[3].name,
    scenario_presets[4].name, "Custom",
};

template <std::size_t Size>
void set_text(std::array<char, Size> &destination,
              const std::string_view value) {
  destination.fill('\0');
  const auto count = std::min(value.size(), Size - 1U);
  std::copy_n(value.data(), count, destination.data());
}

[[nodiscard]] std::string trim_copy(const std::string_view value) {
  const auto not_space = [](const char character) {
    return std::isspace(static_cast<unsigned char>(character)) == 0;
  };
  const auto first = std::find_if(value.begin(), value.end(), not_space);
  const auto last =
      std::find_if(value.rbegin(), value.rend(), not_space).base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

[[nodiscard]] std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const char character) {
                   return static_cast<char>(
                       std::tolower(static_cast<unsigned char>(character)));
                 });
  return value;
}

[[nodiscard]] bool matches_filter(const agent::WorldEvent &event,
                                  const std::string_view filter) {
  if (filter.empty()) {
    return true;
  }
  auto searchable = std::to_string(event.tick) + " " +
                    std::to_string(event.turn) + " " + event.tool + " " +
                    event.arguments.dump() + " " + event.result.dump();
  return lowercase(std::move(searchable))
             .find(lowercase(std::string{filter})) != std::string::npos;
}

[[nodiscard]] std::string compact(const nlohmann::json &value,
                                  const std::size_t maximum = 150U) {
  auto text = value.dump();
  if (text.size() > maximum) {
    text.resize(maximum - 3U);
    text += "...";
  }
  return text;
}

[[nodiscard]] ImU32 item_color(const world::ItemType item,
                               const int alpha = 255) noexcept {
  switch (item) {
  case world::ItemType::berry:
    return IM_COL32(177, 92, 255, alpha);
  case world::ItemType::apple:
    return IM_COL32(244, 76, 92, alpha);
  case world::ItemType::truffle:
    return IM_COL32(242, 187, 69, alpha);
  case world::ItemType::toadstool:
    return IM_COL32(231, 95, 63, alpha);
  }
  return IM_COL32(220, 220, 220, alpha);
}

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

[[nodiscard]] std::string decoded_call_label(const agent::WorldEvent &event) {
  auto label = event.tool;
  if (event.arguments.contains("direction") &&
      event.arguments.at("direction").is_string()) {
    label += " " + event.arguments.at("direction").get<std::string>();
  }
  const auto *payload =
      event.result.contains("result") && event.result.at("result").is_object()
          ? &event.result.at("result")
          : nullptr;
  const auto world_failure = payload != nullptr && payload->contains("ok") &&
                             payload->at("ok").is_boolean() &&
                             !payload->at("ok").get<bool>();
  if (!event.action_executed || world_failure) {
    label += " (failed";
    std::optional<std::string> reason;
    if (!event.action_executed && event.result.contains("error") &&
        event.result.at("error").is_object() &&
        event.result.at("error").contains("code") &&
        event.result.at("error").at("code").is_string()) {
      reason = event.result.at("error").at("code").get<std::string>();
    } else if (world_failure && payload->contains("reason") &&
               payload->at("reason").is_string()) {
      reason = payload->at("reason").get<std::string>();
    }
    if (reason) {
      label += ": " + *reason;
    }
    label += ")";
  }
  return label;
}

[[nodiscard]] std::string preset_variant(const int preset) {
  if (preset >= 0 && preset < custom_preset) {
    return scenario_presets[static_cast<std::size_t>(preset)].variant;
  }
  return "custom";
}

} // namespace

AppUi::AppUi(const agent::Config &initial_config) {
  set_text(base_url_, initial_config.base_url);
  set_text(model_, initial_config.model);
  seed_ = initial_config.seed;
  turn_budget_ = static_cast<int>(initial_config.turn_budget);
  max_tool_rounds_ = static_cast<int>(initial_config.max_tool_rounds);
  max_output_tokens_ = initial_config.max_output_tokens;
  temperature_ = initial_config.temperature;
  known_item_values_ = initial_config.known_item_values;
  reward_feedback_ = initial_config.reward_feedback;
  opaque_look_ = initial_config.opaque_look;

  std::random_device entropy;
  const auto clock_value =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto clock_seed = static_cast<std::uint64_t>(clock_value);
  const auto entropy_seed = static_cast<std::uint64_t>(entropy());
  reroll_rng_.seed(clock_seed ^ (entropy_seed << 32U) ^ entropy_seed);

  if (initial_config.model.empty()) {
    status_message_ =
        "Enter a model identifier in Controls, then press Play or Reset.";
  } else {
    static_cast<void>(recreate_session(true));
  }
}

void AppUi::pump(const double now_seconds) {
  if (!session_) {
    return;
  }
  pump_stats_ = session_->pump();
  animation_.set_speed(animation_speed_);
  animation_.update(session_->events(), session_->world().position(),
                    now_seconds);
  if (!session_->metrics_error().empty()) {
    visible_error_ = "Metrics error: " + session_->metrics_error();
  }
}

void AppUi::draw(const double /*now_seconds*/) {
  const auto dockspace = ImGui::DockSpaceOverViewport(
      0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
  if (!default_layout_built_) {
    build_default_dock_layout(dockspace);
    default_layout_built_ = true;
  }

  draw_world_panel();
  draw_transcript_panel();
  draw_event_log_panel();
  draw_controls_panel();
  draw_stats_panel();
  draw_guidance_panel();
}

agent::Config AppUi::config_from_controls() const {
  return {
      .base_url = trim_copy(base_url_.data()),
      .model = trim_copy(model_.data()),
      .seed = seed_,
      .turn_budget =
          static_cast<std::size_t>(std::clamp(turn_budget_, 1, 10'000)),
      .max_tool_rounds =
          static_cast<std::uint32_t>(std::clamp(max_tool_rounds_, 1, 64)),
      .max_output_tokens = max_output_tokens_,
      .temperature = std::clamp(temperature_, 0.0, 2.0),
      .known_item_values = known_item_values_,
      .reward_feedback = reward_feedback_,
      .opaque_look = opaque_look_,
  };
}

bool AppUi::recreate_session(const bool auto_play) {
  const auto selected_config = config_from_controls();
  turn_budget_ = static_cast<int>(selected_config.turn_budget);
  max_tool_rounds_ = static_cast<int>(selected_config.max_tool_rounds);
  temperature_ = selected_config.temperature;
  auto created =
      agent::Session::create(selected_config, "logs", preset_variant(preset_));
  if (!created) {
    visible_error_ = "Could not create session: " + created.error();
    return false;
  }

  if (session_) {
    static_cast<void>(session_->stop());
  }
  session_ = std::move(*created);
  animation_.reset(session_->world().position());
  pump_stats_ = {};
  transcript_fingerprint_ = 0U;
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
  return true;
}

void AppUi::apply_preset(const int preset) {
  if (preset < 0 || preset >= custom_preset) {
    return;
  }
  const auto &selected = scenario_presets[static_cast<std::size_t>(preset)];
  known_item_values_ = selected.known_item_values;
  reward_feedback_ = selected.reward_feedback;
  opaque_look_ = selected.opaque_look;
}

void AppUi::queue_guidance() {
  const auto message = trim_copy(guidance_.data());
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
  static_cast<void>(session_->queue_user_input(message));
  guidance_.fill('\0');
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
    const auto pulse = std::sin(effect->progress * pi);
    const auto alpha = static_cast<int>(80.0F + pulse * 175.0F);
    const auto origin = cell_center(static_cast<float>(effect->origin.x),
                                    static_cast<float>(effect->origin.y));
    if (effect->kind == VisualEffectKind::look && effect->direction) {
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
    } else if (effect->kind == VisualEffectKind::eat) {
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
  if (session_->events().empty()) {
    ImGui::TextDisabled("Last decoded call: none");
  } else {
    const auto last_call = decoded_call_label(session_->events().back());
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
  ImGui::TextDisabled("Decoded world-tool calls are sourced from world events");
  ImGui::Separator();

  if (!session_) {
    ImGui::TextDisabled("No transcript yet.");
    ImGui::End();
    return;
  }

  const auto &transcript = session_->runner().transcript();
  const auto &events = session_->events();
  const auto snapshot = session_->runner().snapshot();
  auto fingerprint = transcript.size() * 131U + events.size();
  if (!transcript.empty()) {
    fingerprint += transcript.back().text.size();
  }
  const auto should_scroll = fingerprint != transcript_fingerprint_;
  transcript_fingerprint_ = fingerprint;

  ImGui::BeginChild("transcript-scroll", {0.0F, 0.0F}, ImGuiChildFlags_Borders);
  if (transcript.empty()) {
    ImGui::TextDisabled("The first automatic turn will appear here.");
  }
  for (std::size_t index = 0; index < transcript.size(); ++index) {
    const auto &entry = transcript[index];
    ImGui::PushID(static_cast<int>(index));
    if (entry.role == agent::TranscriptRole::assistant) {
      ImGui::TextColored({0.95F, 0.78F, 0.32F, 1.0F},
                         "Turn %u · Decoded world-tool calls", entry.turn);
      auto decoded_calls = 0U;
      for (const auto &event : events) {
        if (event.turn != entry.turn) {
          continue;
        }
        ++decoded_calls;
        const auto arguments = compact(event.arguments, 72U);
        const auto result = compact(event.result, 110U);
        ImGui::TextColored({0.95F, 0.78F, 0.32F, 1.0F}, "  %s",
                           event.tool.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s -> %s", arguments.c_str(), result.c_str());
      }
      if (decoded_calls == 0U) {
        const auto active_turn = snapshot.turns_used + 1U;
        if (snapshot.turn_in_flight && entry.turn == active_turn) {
          ImGui::TextDisabled("  No decoded world-tool calls yet");
        } else {
          ImGui::TextColored({1.0F, 0.52F, 0.32F, 1.0F},
                             "  No decoded world-tool calls");
        }
      }
      ImGui::TextColored(role_color(entry.role), "Model narration");
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
    ImGui::PopID();
  }
  if (should_scroll && transcript_auto_scroll_) {
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
                           event_filter_.data(), event_filter_.size());
  ImGui::SameLine();
  ImGui::TextDisabled("%zu decoded calls",
                      session_ ? session_->events().size() : 0U);

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
    for (const auto &event : session_->events()) {
      if (!matches_filter(event, trim_copy(event_filter_.data()))) {
        continue;
      }
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%llu", static_cast<unsigned long long>(event.tick));
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%zu", event.turn);
      ImGui::TableSetColumnIndex(2);
      ImGui::TextColored({0.85F, 0.72F, 0.35F, 1.0F}, "%s", event.tool.c_str());

      const auto arguments_full = event.arguments.dump();
      const auto result_full = event.result.dump();
      const auto arguments_short = compact(event.arguments);
      const auto result_short = compact(event.result);
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(arguments_short.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", arguments_full.c_str());
      }
      ImGui::TableSetColumnIndex(4);
      const auto *payload = event.result.contains("result") &&
                                    event.result.at("result").is_object()
                                ? &event.result.at("result")
                                : nullptr;
      const auto failed =
          !event.action_executed ||
          (payload != nullptr && payload->contains("ok") &&
           payload->at("ok").is_boolean() && !payload->at("ok").get<bool>());
      if (failed) {
        ImGui::TextColored({1.0F, 0.58F, 0.35F, 1.0F}, "%s",
                           result_short.c_str());
      } else {
        ImGui::TextUnformatted(result_short.c_str());
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", result_full.c_str());
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
  ImGui::InputText("Base URL", base_url_.data(), base_url_.size());
  ImGui::InputText("Model (required)", model_.data(), model_.size());

  ImGui::SeparatorText("Scenario");
  auto selected_preset = preset_;
  if (ImGui::Combo("Preset", &selected_preset, preset_names.data(),
                   static_cast<int>(preset_names.size()))) {
    preset_ = selected_preset;
    apply_preset(preset_);
  }
  if (ImGui::Checkbox("Known item values", &known_item_values_)) {
    preset_ = custom_preset;
  }
  if (ImGui::Checkbox("Reward feedback", &reward_feedback_)) {
    preset_ = custom_preset;
  }
  if (ImGui::Checkbox("Opaque look", &opaque_look_)) {
    preset_ = custom_preset;
  }

  ImGui::InputScalar("Seed", ImGuiDataType_U64, &seed_);
  ImGui::SameLine();
  if (ImGui::Button("Reroll + Reset")) {
    seed_ = reroll_rng_();
    static_cast<void>(recreate_session(true));
  }
  ImGui::InputInt("Turn budget", &turn_budget_);
  ImGui::InputInt("Tool rounds / turn", &max_tool_rounds_);
  ImGui::InputDouble("Temperature", &temperature_, 0.1, 0.5, "%.2f");
  temperature_ = std::clamp(temperature_, 0.0, 2.0);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Sampling control from 0.0 to 2.0; applies on Reset.");
  }
  ImGui::SliderFloat("Animation speed", &animation_speed_, 0.25F, 4.0F,
                     "%.2fx");
  animation_.set_speed(animation_speed_);

  ImGui::SeparatorText("Episode");
  const auto snapshot =
      session_ ? session_->runner().snapshot() : agent::EpisodeSnapshot{};
  const auto can_play = !session_ || snapshot.state == agent::RunState::idle ||
                        snapshot.state == agent::RunState::paused;
  ImGui::BeginDisabled(!can_play);
  if (ImGui::Button(snapshot.state == agent::RunState::paused ? "Resume"
                                                              : "Play")) {
    if (!session_ && !recreate_session(false)) {
      // recreate_session already provided a visible error.
    } else if (session_ && !session_->play()) {
      visible_error_ = "The current episode cannot enter playing state.";
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!session_ || snapshot.state != agent::RunState::playing);
  if (ImGui::Button("Pause") && !session_->pause()) {
    visible_error_ = "Pause takes effect only while an episode is playing.";
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!session_ ||
                       snapshot.state == agent::RunState::finished);
  if (ImGui::Button("Stop") && !session_->stop()) {
    visible_error_ = "The current episode could not be stopped.";
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    static_cast<void>(recreate_session(true));
  }

  if (session_) {
    ImGui::Text("State: %s", agent::run_state_name(snapshot.state).data());
    if (config_from_controls() != session_->config()) {
      ImGui::TextColored({1.0F, 0.78F, 0.28F, 1.0F},
                         "Pending settings apply on Reset.");
    }
    ImGui::TextWrapped("Metrics: %s",
                       session_->metrics_path().string().c_str());
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

  std::size_t moves{};
  std::size_t looks{};
  std::size_t eat_attempts{};
  std::size_t successful_eats{};
  std::size_t failed_eats{};
  for (const auto &event : session_->events()) {
    if (event.tool == "move") {
      ++moves;
    } else if (event.tool == "look") {
      ++looks;
    } else if (event.tool == "eat") {
      ++eat_attempts;
      if (event.eaten) {
        ++successful_eats;
      } else {
        ++failed_eats;
      }
    }
  }

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
        std::pair{"move", moves},
        std::pair{"look", looks},
        std::pair{"eat attempts", eat_attempts},
        std::pair{"decoded calls", session_->tool_call_count()},
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

  ImGui::Text("Eat attempts %zu", eat_attempts);
  ImGui::SameLine();
  ImGui::TextColored({0.46F, 0.92F, 0.62F, 1.0F}, "| successful %zu",
                     successful_eats);
  ImGui::SameLine();
  ImGui::TextColored({1.0F, 0.52F, 0.32F, 1.0F}, "| failed %zu", failed_eats);

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
      "##guidance", "e.g. Search the western edge before eating.",
      guidance_.data(), guidance_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const auto clicked = ImGui::Button("Queue", {button_width, 0.0F});
  if (submitted || clicked) {
    queue_guidance();
  }
  if (session_) {
    auto has_pending = false;
    for (const auto &entry : session_->runner().guidance()) {
      has_pending =
          has_pending || entry.status == agent::GuidanceStatus::pending;
    }
    if (has_pending) {
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
