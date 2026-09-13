/// @file app_ui.hpp
/// @brief The Dear ImGui front end: session ownership and panel drawing.
///
/// AppUi owns the shared_ptr<agent::Session> and all widget state; the GUI
/// entry point only constructs it and calls pump() and draw() each frame.
#pragma once

#include "agent/config.hpp"
#include "agent/session.hpp"
#include "ui/activity_history.hpp"
#include "ui/world_animation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace pigpen::ui {

/// @brief The GUI application: session, control state, dockable panels.
///
/// A Session is the reset unit, so connection and scenario edits never touch
/// a live episode; they take effect when Reset rebuilds the session wholesale.
class AppUi final {
public:
  /// @brief Seeds the controls from @p initial_config; a non-empty model
  /// identifier creates and auto-starts a session immediately.
  explicit AppUi(const agent::Config &initial_config = {});

  /// @brief Advances the session and the animation timeline.
  /// @param now_seconds Monotonic time supplied by the frame loop.
  /// @note Call once per frame, before draw().
  void pump(double now_seconds);

  /// @brief Emits every panel for the current ImGui frame.
  void draw(double now_seconds);

private:
  /// @brief Snapshots the control widgets into a validated agent::Config.
  [[nodiscard]] agent::Config config_from_controls() const;
  /// @brief Destroys the current session and builds a new one from the
  /// controls; the only way settings changes are applied.
  [[nodiscard]] bool recreate_session(bool auto_play);
  /// @brief Applies a scenario preset to the three model-visibility flags.
  void apply_preset(int preset);
  /// @brief Queues the guidance text for delivery on a future model turn.
  void queue_guidance();
  /// @brief Splits the dockspace into the default panel layout, first run
  /// only; afterwards imgui.ini owns the layout.
  void build_default_dock_layout(std::uint32_t dockspace_id);

  void draw_world_panel();
  void draw_transcript_panel();
  void draw_event_log_panel();
  void draw_controls_panel();
  void draw_stats_panel();
  void draw_guidance_panel();

  std::shared_ptr<agent::Session> session_{};
  WorldAnimationState animation_{};
  ActivityHistory activity_history_{};
  agent::PumpStats pump_stats_{};

  std::array<char, 384> base_url_{};
  std::array<char, 160> model_{};
  std::array<char, 160> event_filter_{};
  std::array<char, 1024> guidance_{};
  std::uint64_t seed_{};
  int turn_budget_{20};
  int max_tool_rounds_{8};
  std::uint32_t max_output_tokens_{8'096};
  double temperature_{};
  int preset_{0};
  bool known_item_values_{true};
  bool reward_feedback_{true};
  bool opaque_look_{false};
  bool transcript_auto_scroll_{true};
  float animation_speed_{1.0F};

  std::mt19937_64 reroll_rng_{};
  std::string status_message_{};
  std::string visible_error_{};
  std::vector<float> transcript_entry_heights_{};
  std::vector<std::size_t> transcript_text_lengths_{};
  std::vector<std::size_t> transcript_activity_counts_{};
  const void *transcript_font_{};
  float transcript_layout_width_{};
  float transcript_line_height_{};
  std::array<std::size_t, 3> transcript_fingerprint_{};
  bool dock_layout_initialized_{false};
};

} // namespace pigpen::ui
