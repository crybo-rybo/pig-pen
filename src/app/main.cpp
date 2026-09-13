/// @file main.cpp
/// @brief GUI entry point: SDL3/OpenGL/ImGui setup and the frame loop.
///
/// Nothing application-specific lives here — panels, controls, and session
/// ownership belong to ui::AppUi, which this loop pumps and draws once per
/// frame.
#include "ui/app_ui.hpp"
#include "ui/gui_options.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] double current_time_seconds() {
  constexpr auto nanoseconds_per_second = 1'000'000'000.0;
  return static_cast<double>(SDL_GetTicksNS()) / nanoseconds_per_second;
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

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "Could not initialize SDL3: %s\n", SDL_GetError());
    return 1;
  }

#if defined(__APPLE__)
  constexpr auto glsl_version = "#version 150";
  constexpr auto context_flags = SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
  constexpr auto context_minor_version = 2;
#else
  constexpr auto glsl_version = "#version 330";
  constexpr auto context_flags = 0;
  constexpr auto context_minor_version = 3;
#endif
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, context_flags);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, context_minor_version);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  constexpr auto window_flags =
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  auto *window = SDL_CreateWindow("pig-pen", 1440, 900, window_flags);
  if (window == nullptr) {
    std::fprintf(stderr, "Could not create the pig-pen window: %s\n",
                 SDL_GetError());
    SDL_Quit();
    return 1;
  }
  const auto gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    std::fprintf(stderr, "Could not create the OpenGL context: %s\n",
                 SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (!SDL_GL_MakeCurrent(window, gl_context)) {
    std::fprintf(stderr, "Could not activate the OpenGL context: %s\n",
                 SDL_GetError());
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (!SDL_GL_SetSwapInterval(1)) {
    std::fprintf(stderr, "Warning: could not enable vertical sync: %s\n",
                 SDL_GetError());
  }

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

  if (!ImGui_ImplSDL3_InitForOpenGL(window, gl_context)) {
    std::fprintf(stderr, "Could not initialize the ImGui SDL3 backend\n");
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
    std::fprintf(stderr, "Could not initialize the ImGui OpenGL backend\n");
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  int exit_code{};
  {
    pigpen::ui::AppUi application{options->config};
    bool done{};
    while (!done) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
             event.window.windowID == SDL_GetWindowID(window))) {
          done = true;
        }
      }
      if (done) {
        break;
      }

      const auto current_time = current_time_seconds();
      application.pump(current_time);
      if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0U) {
        SDL_Delay(10);
        continue;
      }

      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();
      application.draw(current_time);

      ImGui::Render();
      int framebuffer_width{};
      int framebuffer_height{};
      if (!SDL_GetWindowSizeInPixels(window, &framebuffer_width,
                                     &framebuffer_height)) {
        std::fprintf(stderr, "Could not query the window size: %s\n",
                     SDL_GetError());
        exit_code = 1;
        break;
      }
      glViewport(0, 0, framebuffer_width, framebuffer_height);
      glClearColor(0.035F, 0.045F, 0.065F, 1.0F);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (!SDL_GL_SwapWindow(window)) {
        std::fprintf(stderr, "Could not present the OpenGL frame: %s\n",
                     SDL_GetError());
        exit_code = 1;
        break;
      }
    }
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return exit_code;
}
