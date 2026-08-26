cmake_minimum_required(VERSION 3.25)

project(
  pig-pen
  VERSION 0.1.0
  DESCRIPTION "A small observable world for a locally hosted LLM"
  LANGUAGES CXX
)

# C++26 reflection is Pig Pen's model-facing type system, not an optional
# enhancement. Scry's supported P2996/P3394 implementation currently requires
# GCC 16 or newer and its explicit -freflection gate.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message(FATAL_ERROR "Pig Pen requires GCC 16 or newer with C++26 reflection")
endif()

option(PIGPEN_BUILD_GUI "Build the Dear ImGui application" ON)
option(PIGPEN_BUILD_TESTS "Build pig-pen's test suite" ON)
option(PIGPEN_WARNINGS_AS_ERRORS "Treat warnings in pig-pen code as errors" ON)
set(
  PIGPEN_SCRY_SOURCE
  ""
  CACHE PATH
  "Optional path to a local scry checkout (the pinned revision is fetched when empty)"
)

include(cmake/dependencies.cmake)

# Everything Pig Pen's own targets need: the language level, the src/-rooted
# include convention, and warnings. Fetched dependencies are SYSTEM and never
# link this, so they are unaffected.
add_library(pigpen_defaults INTERFACE)
target_compile_features(pigpen_defaults INTERFACE cxx_std_26)
target_include_directories(
  pigpen_defaults
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_options(
  pigpen_defaults
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)
if(PIGPEN_WARNINGS_AS_ERRORS)
  target_compile_options(pigpen_defaults INTERFACE -Werror)
endif()

function(pigpen_target target)
  target_link_libraries(${target} PRIVATE pigpen_defaults)
endfunction()

add_library(pigpen_world STATIC src/world/world.cpp)
pigpen_target(pigpen_world)

add_library(pigpen_reflected_tools STATIC src/agent/world_tools.cpp)
target_link_libraries(pigpen_reflected_tools PUBLIC pigpen_world scry::reflection)
pigpen_target(pigpen_reflected_tools)

add_library(pigpen_agent STATIC
  src/agent/episode_runner.cpp
  src/agent/metrics_writer.cpp
  src/agent/prompt.cpp
  src/agent/scry_transport.cpp
  src/agent/session.cpp
)
target_link_libraries(
  pigpen_agent
  PUBLIC
    pigpen_world
  PRIVATE
    nlohmann_json::nlohmann_json
    pigpen_reflected_tools
    scry::scry
)
pigpen_target(pigpen_agent)

# UI logic shared by the GUI application and the test suite (kept ImGui-free so
# tests build without SDL/OpenGL).
add_library(pigpen_ui STATIC src/ui/gui_options.cpp src/ui/world_animation.cpp)
pigpen_target(pigpen_ui)

add_executable(pig-pen-headless src/app/headless_main.cpp)
target_link_libraries(pig-pen-headless PRIVATE pigpen_agent)
pigpen_target(pig-pen-headless)

if(PIGPEN_BUILD_GUI)
  find_package(OpenGL REQUIRED)

  add_library(
    pigpen_imgui STATIC
      "${imgui_SOURCE_DIR}/imgui.cpp"
      "${imgui_SOURCE_DIR}/imgui_draw.cpp"
      "${imgui_SOURCE_DIR}/imgui_tables.cpp"
      "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
  )
  target_include_directories(
    pigpen_imgui
    SYSTEM PUBLIC
      "${imgui_SOURCE_DIR}"
      "${imgui_SOURCE_DIR}/backends"
  )
  target_link_libraries(pigpen_imgui PUBLIC SDL3::SDL3 OpenGL::GL)
  target_compile_definitions(
    pigpen_imgui
    PUBLIC $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>
  )

  add_executable(pig-pen src/app/main.cpp src/ui/app_ui.cpp)
  target_link_libraries(pig-pen PRIVATE pigpen_agent pigpen_imgui pigpen_ui)
  pigpen_target(pig-pen)
endif()

if(PIGPEN_BUILD_TESTS)
  include(cmake/testing.cmake)
endif()
