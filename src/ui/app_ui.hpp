/// @file app_ui.hpp
/// @brief The Dear ImGui front end: session ownership and panel drawing.
#pragma once

#include "agent/config.hpp"
#include "agent/session.hpp"
#include "ui/world_animation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace pigpen::ui {

/// @brief The GUI application: session, control state, dockable panels. The
/// GUI entry point only constructs it and calls pump() and draw() each frame.
///
/// A Session is the reset unit, so connection and scenario edits never touch
/// a live episode; they take effect when Reset rebuilds the session wholesale.
class AppUi final {
public:
  /// @brief Seeds the controls from @p initial_config; a non-empty model
  /// identifier creates and auto-starts a session immediately.
  explicit AppUi(const agent::Config &initial_config);

  /// @brief Advances the session and the animation timeline; call once per
  /// frame, before draw().
  /// @param now_seconds Monotonic time supplied by the frame loop.
  void pump(double now_seconds);

  /// @brief Emits every panel for the current ImGui frame.
  void draw();

private:
  /// @brief The Config the controls describe; Session::create validates it.
  [[nodiscard]] agent::Config config_from_controls() const;
  /// @brief Replaces the session with one built from the controls — the only
  /// way settings changes are applied. Failures surface in visible_error_.
  void recreate_session(bool auto_play);
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

  /// Settings for the next session. turn_budget, max_tool_rounds, and
  /// sampling_seed are edited through the widget mirrors below instead.
  agent::Config controls_{};
  int turn_budget_{};
  int max_tool_rounds_{};
  bool use_sampling_seed_{};
  std::uint32_t sampling_seed_{};
  /// Index into the scenario presets; nullopt once a flag is edited by hand.
  std::optional<std::size_t> preset_{0};

  std::string event_filter_{};
  std::string guidance_{};
  bool transcript_auto_scroll_{true};
  float animation_speed_{1.0F};

  std::string status_message_{};
  std::string visible_error_{};
  bool dock_layout_checked_{false};
};

} // namespace pigpen::ui
