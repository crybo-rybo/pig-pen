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
  "Deprecated path to a local scry checkout; prefer FETCHCONTENT_SOURCE_DIR_SCRY"
)
include(cmake/dependencies.cmake)

# Let newer CMake versions de-duplicate static libraries according to the
# selected linker's capabilities. Older supported versions retain their
# established ordering behavior.
if(POLICY CMP0156)
  cmake_policy(SET CMP0156 NEW)
endif()

# Pig Pen's warning policy. Fetched dependencies are SYSTEM and never link
# this, so they are unaffected; public consumers also do not inherit it.
add_library(pigpen_defaults INTERFACE)
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
  set(pigpen_usage_scope PRIVATE)
  if(ARGC GREATER 1)
    if(NOT ARGC EQUAL 2 OR NOT ARGV1 STREQUAL "PUBLIC")
      message(
        FATAL_ERROR
        "pigpen_target accepts only an optional PUBLIC argument"
      )
    endif()
    set(pigpen_usage_scope PUBLIC)
  endif()

  target_link_libraries(${target} PRIVATE pigpen_defaults)
  target_compile_features(${target} ${pigpen_usage_scope} cxx_std_26)
  target_include_directories(
    ${target}
    ${pigpen_usage_scope} "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
  set_target_properties(
    ${target}
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
  )
endfunction()

add_library(pigpen_world STATIC src/world/world.cpp)
pigpen_target(pigpen_world PUBLIC)

add_library(pigpen_reflected_tools STATIC src/agent/world_tools.cpp)
target_link_libraries(pigpen_reflected_tools PUBLIC pigpen_world scry::reflection)
pigpen_target(pigpen_reflected_tools PUBLIC)

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
pigpen_target(pigpen_agent PUBLIC)

# A build-time exporter uses the same compiled prompt catalog and reflected
# tool bindings as Session. The JSON is a reference artifact, not runtime input.
add_executable(pigpen-resource-manifest src/app/resource_manifest_main.cpp)
target_link_libraries(
  pigpen-resource-manifest
  PRIVATE pigpen_agent pigpen_reflected_tools nlohmann_json::nlohmann_json
)
target_compile_definitions(
  pigpen-resource-manifest
  PRIVATE
    PIGPEN_VERSION="${PROJECT_VERSION}"
    PIGPEN_BUILD_CONFIGURATION="$<CONFIG>"
)
pigpen_target(pigpen-resource-manifest)
set(PIGPEN_RESOURCE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/resources/build-manifest.json")
add_custom_command(
  OUTPUT "${PIGPEN_RESOURCE_MANIFEST}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/resources"
  COMMAND pigpen-resource-manifest "${PIGPEN_RESOURCE_MANIFEST}"
  DEPENDS pigpen-resource-manifest
  COMMENT "Exporting compiled prompts and reflected tool schemas"
  VERBATIM
)
add_custom_target(pigpen_resources ALL DEPENDS "${PIGPEN_RESOURCE_MANIFEST}")

# UI logic shared by the GUI application and the test suite (kept ImGui-free so
# tests build without SDL/OpenGL).
if(PIGPEN_BUILD_GUI OR PIGPEN_BUILD_TESTS)
  add_library(pigpen_ui STATIC src/ui/gui_options.cpp src/ui/world_animation.cpp)
  target_link_libraries(pigpen_ui PUBLIC pigpen_agent pigpen_world)
  pigpen_target(pigpen_ui PUBLIC)
endif()

add_executable(pig-pen-headless src/app/headless_main.cpp)
target_link_libraries(pig-pen-headless PRIVATE pigpen_agent)
pigpen_target(pig-pen-headless)
add_dependencies(pig-pen-headless pigpen_resources)

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
  add_dependencies(pig-pen pigpen_resources)
endif()

if(PIGPEN_BUILD_TESTS)
  include(cmake/testing.cmake)
endif()
