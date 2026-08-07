#include "ui/app_ui.hpp"
#include "ui/gui_options.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void glfw_error_callback(const int error, const char *description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error,
               description == nullptr ? "unknown error" : description);
}

void print_usage(std::ostream &output, const std::string_view program) {
  output << "Usage: " << program << " [--model NAME] [options]\n\n"
         << "Open the pig-pen GUI, optionally creating and starting a session "
            "immediately.\n\n"
         << "Options:\n"
         << "  --model NAME    Populate the model field and auto-start\n"
         << "  --base-url URL  Populate the model endpoint field\n"
         << "                  (default: http://127.0.0.1:11434/v1)\n"
         << "  --help          Show this help and exit\n\n"
         << "Values may also use --option=value. Without --model, the GUI "
            "waits for\n"
         << "a model identifier to be entered in Controls.\n";
}

} // namespace

int main(const int argc, char **argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  auto options = pigpen::ui::parse_gui_options(arguments);
  if (!options) {
    std::cerr << "option error: " << options.error() << "\n\n";
    print_usage(std::cerr, argc > 0 ? argv[0] : "pig-pen");
    return 2;
  }
  if (options->help) {
    print_usage(std::cout, argc > 0 ? argv[0] : "pig-pen");
    return 0;
  }

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
    pigpen::ui::AppUi application{options->config};
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
