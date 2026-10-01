cmake_minimum_required(VERSION 3.31)

project(
  pig-pen
  VERSION 0.1.0
  DESCRIPTION "A small observable world for a locally hosted LLM"
  LANGUAGES CXX
)

# C++26 reflection is Pig Pen's model-facing type system, and Scry's public API
# uses it too: linking scry::scry carries -freflection. Scry's supported
# P2996/P3394 implementation currently requires GCC 16 or newer.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message(FATAL_ERROR "Pig Pen requires GCC 16 or newer with C++26 reflection")
endif()

# Pig Pen uses no C++ modules; skip the per-TU module dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(PIGPEN_BUILD_GUI "Build the Dear ImGui application" ON)
option(PIGPEN_BUILD_TESTS "Build pig-pen's test suite" ON)
option(PIGPEN_WARNINGS_AS_ERRORS "Treat warnings in pig-pen code as errors" ON)
set(
  PIGPEN_SCRY_SOURCE
  ""
  CACHE PATH
  "Deprecated path to a local scry checkout; prefer FETCHCONTENT_SOURCE_DIR_SCRY"
)
include(cmake/dependencies.cmake)

# Standard, include root, and warnings for a pig-pen target. Fetched
# dependencies are SYSTEM and never get these warnings. Everything is C++23
# except the reflection layer, which asks for C++26 privately so the standard
# never leaks into the targets that link it.
function(pigpen_target target)
  target_compile_features(${target} PUBLIC cxx_std_23)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/src")
  target_compile_options(
    ${target}
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            $<$<BOOL:${PIGPEN_WARNINGS_AS_ERRORS}>:-Werror>
  )
endfunction()

add_library(pigpen_world STATIC src/world/world.cpp)
pigpen_target(pigpen_world)

# The runtime that needs no reflection: the turn loop, prompts, and world
# actions. It never links scry, so a <scry/...> include here cannot compile.
add_library(pigpen_core STATIC
  src/agent/episode_runner.cpp
  src/agent/prompt.cpp
  src/agent/world_tools.cpp
)
target_link_libraries(pigpen_core PUBLIC pigpen_world)
pigpen_target(pigpen_core)

# The reflection layer: the tool contract and toolbox, the scry transport, the
# reflected JSONL writer, and the Session that composes them. Every TU here
# compiles as C++26 with -freflection (carried by scry::scry). Scry stays
# PRIVATE, and so does the C++26 requirement, so everything that links this
# library, including both front ends, stays C++23 and reflection-free.
add_library(pigpen_agent STATIC
  src/agent/metrics_writer.cpp
  src/agent/scry_transport.cpp
  src/agent/session.cpp
  src/agent/world_tool_binding.cpp
)
target_link_libraries(pigpen_agent PUBLIC pigpen_core PRIVATE scry::scry)
pigpen_target(pigpen_agent)
target_compile_features(pigpen_agent PRIVATE cxx_std_26)

# ImGui-free UI logic shared by the GUI and the tests.
if(PIGPEN_BUILD_GUI OR PIGPEN_BUILD_TESTS)
  add_library(pigpen_ui STATIC src/ui/gui_options.cpp src/ui/world_animation.cpp)
  target_link_libraries(pigpen_ui PUBLIC pigpen_core)
  pigpen_target(pigpen_ui)
endif()

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
      "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
  )
  target_include_directories(
    pigpen_imgui
    SYSTEM PUBLIC
      "${imgui_SOURCE_DIR}"
      "${imgui_SOURCE_DIR}/backends"
      "${imgui_SOURCE_DIR}/misc/cpp"
  )
  target_link_libraries(pigpen_imgui PUBLIC SDL3::SDL3 OpenGL::GL)
  target_compile_definitions(
    pigpen_imgui
    PUBLIC $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>
  )

  add_executable(pig-pen src/app/main.cpp src/ui/app_ui.cpp)
  target_link_libraries(pig-pen PRIVATE pigpen_imgui pigpen_ui pigpen_agent)
  pigpen_target(pig-pen)
endif()

if(PIGPEN_BUILD_TESTS)
  include(cmake/testing.cmake)
endif()
