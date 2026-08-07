#include "ui/app_ui.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>

namespace {

void glfw_error_callback(const int error, const char *description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error,
               description == nullptr ? "unknown error" : description);
}

} // namespace

int main() {
  glfwSetErrorCallback(glfw_error_callback);
  if (glfwInit() == GLFW_FALSE) {
    std::fprintf(stderr, "Could not initialize GLFW\n");
    return 1;
  }

  constexpr auto glsl_version = "#version 330";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

  auto *window = glfwCreateWindow(1440, 900, "pig-pen", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "Could not create the pig-pen window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.WindowRounding = 5.0F;
  style.FrameRounding = 4.0F;
  style.GrabRounding = 4.0F;
  style.TabRounding = 4.0F;

  if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
    std::fprintf(stderr, "Could not initialize the ImGui GLFW backend\n");
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
  if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
    std::fprintf(stderr, "Could not initialize the ImGui OpenGL backend\n");
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  {
    pigpen::ui::AppUi application;
    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
      glfwPollEvents();
      application.pump(glfwGetTime());

      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      application.draw(glfwGetTime());

      ImGui::Render();
      int framebuffer_width{};
      int framebuffer_height{};
      glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
      glViewport(0, 0, framebuffer_width, framebuffer_height);
      glClearColor(0.035F, 0.045F, 0.065F, 1.0F);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      glfwSwapBuffers(window);
    }
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
