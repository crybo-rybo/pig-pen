#include "ui/app_ui.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>
#include <string>

namespace {

constexpr std::array panel_names{"World",    "Transcript", "Event Log",
                                 "Controls", "Stats",      "Guidance"};

class ImGuiContextScope final {
public:
  ImGuiContextScope() : context_{ImGui::CreateContext()} {
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.DisplaySize = {1440.0F, 900.0F};
    unsigned char *pixels{};
    int width{};
    int height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }

  ~ImGuiContextScope() { ImGui::DestroyContext(context_); }

  ImGuiContextScope(const ImGuiContextScope &) = delete;
  ImGuiContextScope &operator=(const ImGuiContextScope &) = delete;

private:
  ImGuiContext *context_{};
};

void render_frame(pigpen::ui::AppUi &application) {
  ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
  ImGui::NewFrame();
  application.draw(0.0);
  ImGui::Render();
}

[[nodiscard]] ImGuiID legacy_dockspace_id() {
  std::array<char, 32> host_window_name{};
  std::snprintf(host_window_name.data(), host_window_name.size(),
                "WindowOverViewport_%08X", ImGui::GetMainViewport()->ID);
  return ImHashStr("DockSpace", 0U, ImHashStr(host_window_name.data()));
}

} // namespace

TEST_CASE("AppUi retains a saved fully undocked layout", "[ui][layout]") {
  std::string saved_layout;
  {
    ImGuiContextScope context;
    pigpen::ui::AppUi application;
    render_frame(application);

    const auto dockspace_id = legacy_dockspace_id();
    auto *root = ImGui::DockBuilderGetNode(dockspace_id);
    REQUIRE(root != nullptr);
    REQUIRE(root->IsSplitNode());

    for (const auto *panel_name : panel_names) {
      auto *window = ImGui::FindWindowByName(panel_name);
      REQUIRE(window != nullptr);
      ImGui::SetWindowDock(window, 0, ImGuiCond_Always);
    }
    ImGui::DockBuilderRemoveNode(dockspace_id);

    std::size_t saved_size{};
    const auto *saved = ImGui::SaveIniSettingsToMemory(&saved_size);
    saved_layout.assign(saved, saved_size);
    REQUIRE(saved_layout.find("[Window][World]") != std::string::npos);
  }

  {
    ImGuiContextScope context;
    ImGui::LoadIniSettingsFromMemory(saved_layout.data(), saved_layout.size());
    pigpen::ui::AppUi application;
    render_frame(application);

    const auto *root = ImGui::DockBuilderGetNode(legacy_dockspace_id());
    REQUIRE(root != nullptr);
    CHECK_FALSE(root->IsSplitNode());
    for (const auto *panel_name : panel_names) {
      const auto *window = ImGui::FindWindowByName(panel_name);
      REQUIRE(window != nullptr);
      CHECK(window->DockId == 0U);
    }
  }
}

TEST_CASE("AppUi retains a customized legacy dock layout", "[ui][layout]") {
  std::string saved_layout;
  ImGuiID stats_dock_id{};
  {
    ImGuiContextScope context;
    pigpen::ui::AppUi application;
    render_frame(application);

    const auto *stats = ImGui::FindWindowByName("Stats");
    const auto *world = ImGui::FindWindowByName("World");
    REQUIRE(stats != nullptr);
    REQUIRE(world != nullptr);
    REQUIRE(stats->DockId != 0U);
    REQUIRE(world->DockId != stats->DockId);
    stats_dock_id = stats->DockId;

    ImGui::DockBuilderDockWindow("World", stats_dock_id);
    ImGui::DockBuilderFinish(legacy_dockspace_id());
    render_frame(application);
    REQUIRE(ImGui::FindWindowByName("World")->DockId == stats_dock_id);

    std::size_t saved_size{};
    const auto *saved = ImGui::SaveIniSettingsToMemory(&saved_size);
    saved_layout.assign(saved, saved_size);
  }

  {
    ImGuiContextScope context;
    ImGui::LoadIniSettingsFromMemory(saved_layout.data(), saved_layout.size());
    pigpen::ui::AppUi application;
    render_frame(application);

    const auto *stats = ImGui::FindWindowByName("Stats");
    const auto *world = ImGui::FindWindowByName("World");
    REQUIRE(stats != nullptr);
    REQUIRE(world != nullptr);
    CHECK(stats->DockId == stats_dock_id);
    CHECK(world->DockId == stats_dock_id);
  }
}
