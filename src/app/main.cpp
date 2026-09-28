/// @file main.cpp
/// @brief GUI entry point: SDL3/OpenGL/ImGui setup and the frame loop.
///
/// Nothing application-specific lives here — panels, controls, and session
/// ownership belong to ui::AppUi, which this loop pumps and draws once per
/// frame.
#include "agent/session_options.hpp"
#include "cli/config_options.hpp"
#include "ui/app_ui.hpp"
#include "ui/gui_options.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// @brief Runs a cleanup callable at scope exit, in reverse declaration order.
template <typename Cleanup> class Defer final {
public:
  explicit Defer(Cleanup cleanup) : cleanup_(std::move(cleanup)) {}
  ~Defer() { cleanup_(); }
  Defer(const Defer &) = delete;
  Defer &operator=(const Defer &) = delete;

private:
  Cleanup cleanup_;
};

[[nodiscard]] double current_time_seconds() {
  constexpr auto nanoseconds_per_second = 1'000'000'000.0;
  return static_cast<double>(SDL_GetTicksNS()) / nanoseconds_per_second;
}

void print_usage(std::ostream &output, const std::string_view program) {
  output << "Usage: " << program << R"( [--model NAME] [options]

Open the pig-pen GUI, optionally creating and starting a session immediately.

Options:
)" << pigpen::ui::gui_options_help()
         << R"(
Values may also use --option=value. Without --model, the GUI waits for
a model identifier to be entered in Controls. PIGPEN_API_KEY supplies an
optional API key. Logs are written to logs/.
)";
}

/// @brief Reports an SDL failure on stderr and yields the error exit code.
[[nodiscard]] int sdl_failure(const std::string_view what) {
  std::cerr << what << ": " << SDL_GetError() << '\n';
  return 1;
}

} // namespace

int main(const int argc, char **argv) {
  const std::string_view program = argc > 0 ? argv[0] : "pig-pen";
  const std::vector<std::string_view> arguments(argv + (argc > 0 ? 1 : 0),
                                                argv + argc);
  const auto options = pigpen::ui::parse_gui_options(arguments);
  if (!options) {
    std::cerr << "option error: " << options.error() << "\n\n";
    print_usage(std::cerr, program);
    return 2;
  }
  if (options->help) {
    print_usage(std::cout, program);
    return 0;
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    return sdl_failure("Could not initialize SDL3");
  }
  const Defer quit_sdl{[] { SDL_Quit(); }};

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
    return sdl_failure("Could not create the pig-pen window");
  }
  const Defer destroy_window{[window] { SDL_DestroyWindow(window); }};

  const auto gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    return sdl_failure("Could not create the OpenGL context");
  }
  const Defer destroy_gl_context{
      [gl_context] { SDL_GL_DestroyContext(gl_context); }};
  if (!SDL_GL_MakeCurrent(window, gl_context)) {
    return sdl_failure("Could not activate the OpenGL context");
  }
  if (!SDL_GL_SetSwapInterval(1)) {
    std::cerr << "Warning: could not enable vertical sync: " << SDL_GetError()
              << '\n';
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  const Defer destroy_imgui{[] { ImGui::DestroyContext(); }};
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
    std::cerr << "Could not initialize the ImGui SDL3 backend\n";
    return 1;
  }
  const Defer shutdown_sdl_backend{[] { ImGui_ImplSDL3_Shutdown(); }};
  if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
    std::cerr << "Could not initialize the ImGui OpenGL backend\n";
    return 1;
  }
  const Defer shutdown_gl_backend{[] { ImGui_ImplOpenGL3_Shutdown(); }};

  // Declared after every guard so the session, and its log footer, is torn
  // down before the ImGui context it draws into.
  pigpen::ui::AppUi application{
      options->config,
      {
          .log_directory = "logs",
          .api_key = pigpen::cli::api_key_from_environment(),
          .reward_weights = options->reward_weights,
      }};
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
           event.window.windowID == SDL_GetWindowID(window))) {
        return 0;
      }
    }
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0U) {
      SDL_Delay(10);
      continue;
    }

    application.pump(current_time_seconds());

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    application.draw();

    ImGui::Render();
    int framebuffer_width{};
    int framebuffer_height{};
    if (!SDL_GetWindowSizeInPixels(window, &framebuffer_width,
                                   &framebuffer_height)) {
      return sdl_failure("Could not query the window size");
    }
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glClearColor(0.035F, 0.045F, 0.065F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!SDL_GL_SwapWindow(window)) {
      return sdl_failure("Could not present the OpenGL frame");
    }
  }
}
