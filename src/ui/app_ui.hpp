#pragma once

#include "agent/config.hpp"
#include "agent/session.hpp"
#include "ui/world_animation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

namespace pigpen::ui {

class AppUi final {
public:
  AppUi();

  void pump(double now_seconds);
  void draw(double now_seconds);

private:
  [[nodiscard]] agent::Config config_from_controls() const;
  [[nodiscard]] bool recreate_session(bool auto_play);
  void apply_preset(int preset);
  void queue_guidance();
  void build_default_dock_layout(std::uint32_t dockspace_id);

  void draw_world_panel();
  void draw_transcript_panel();
  void draw_event_log_panel();
  void draw_controls_panel();
  void draw_stats_panel();
  void draw_guidance_panel();

  std::shared_ptr<agent::Session> session_{};
  WorldAnimationState animation_{};
  agent::PumpStats pump_stats_{};

  std::array<char, 384> base_url_{};
  std::array<char, 160> model_{};
  std::array<char, 160> event_filter_{};
  std::array<char, 1024> guidance_{};
  std::uint64_t seed_{};
  int turn_budget_{20};
  int max_tool_rounds_{8};
  int preset_{0};
  bool known_item_values_{true};
  bool reward_feedback_{true};
  bool opaque_look_{false};
  bool transcript_auto_scroll_{true};
  float animation_speed_{1.0F};

  std::mt19937_64 reroll_rng_{};
  std::string status_message_{};
  std::string visible_error_{};
  std::string last_guidance_{};
  std::size_t transcript_fingerprint_{};
  bool default_layout_built_{false};
};

} // namespace pigpen::ui
