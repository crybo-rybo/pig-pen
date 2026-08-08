cmake_minimum_required(VERSION 3.25)

project(
  pig-pen
  VERSION 0.1.0
  DESCRIPTION "A small observable world for a locally hosted LLM"
  LANGUAGES CXX
)

include(FetchContent)

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

add_library(pigpen_project_options INTERFACE)
target_compile_options(
  pigpen_project_options
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)
if(PIGPEN_WARNINGS_AS_ERRORS)
  target_compile_options(pigpen_project_options INTERFACE -Werror)
endif()

function(pigpen_configure_target target)
  target_link_libraries(${target} PRIVATE pigpen_project_options)
  set_target_properties(
    ${target}
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
  )
endfunction()

# Keep all upstream projects out of the default test/example graph. Warnings are
# still enabled for pig-pen through pigpen_project_options, but are not imposed
# on pinned third-party code.
set(SCRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_FORMAT_CHECK OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(SCRY_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_REFLECTION ON CACHE BOOL "" FORCE)
set(SCRY_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_IMGUI_SHOWCASE OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_LOCAL_MODEL_SMOKE OFF CACHE BOOL "" FORCE)

if(PIGPEN_SCRY_SOURCE)
  get_filename_component(
    PIGPEN_SCRY_SOURCE_ABSOLUTE
    "${PIGPEN_SCRY_SOURCE}"
    ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(NOT EXISTS "${PIGPEN_SCRY_SOURCE_ABSOLUTE}/CMakeLists.txt")
    message(
      FATAL_ERROR
      "PIGPEN_SCRY_SOURCE does not contain CMakeLists.txt: "
      "${PIGPEN_SCRY_SOURCE_ABSOLUTE}"
    )
  endif()
  FetchContent_Declare(
    scry
    SOURCE_DIR "${PIGPEN_SCRY_SOURCE_ABSOLUTE}"
    SYSTEM
    EXCLUDE_FROM_ALL
  )
else()
  FetchContent_Declare(
    scry
    GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
    GIT_TAG 5ea0474ff34b2d598851269e3bf9e0f60913a167
    GIT_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
endif()

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(scry nlohmann_json)

if(NOT TARGET scry::reflection)
  message(FATAL_ERROR "The selected Scry source does not provide scry::reflection")
endif()

add_library(pigpen_world STATIC src/world/world.cpp)
pigpen_configure_target(pigpen_world)
target_compile_features(pigpen_world PUBLIC cxx_std_26)
target_include_directories(
  pigpen_world
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

add_library(pigpen_reflected_tools STATIC src/agent/world_tools.cpp)
target_compile_features(pigpen_reflected_tools PUBLIC cxx_std_26)
target_link_libraries(
  pigpen_reflected_tools
  PUBLIC
    nlohmann_json::nlohmann_json
    pigpen_world
    scry::reflection
)
pigpen_configure_target(pigpen_reflected_tools)
target_include_directories(
  pigpen_reflected_tools
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

add_library(
  pigpen_agent STATIC
    src/agent/episode_runner.cpp
    src/agent/metrics_writer.cpp
    src/agent/prompt.cpp
    src/agent/scry_transport.cpp
    src/agent/session.cpp
)
target_compile_features(pigpen_agent PUBLIC cxx_std_26)
target_link_libraries(
  pigpen_agent
  PUBLIC
    pigpen_world
    nlohmann_json::nlohmann_json
  PRIVATE
    pigpen_reflected_tools
    scry::scry
)
pigpen_configure_target(pigpen_agent)
target_include_directories(
  pigpen_agent
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

add_executable(pig-pen-headless src/app/headless_main.cpp)
target_link_libraries(pig-pen-headless PRIVATE pigpen_agent)
pigpen_configure_target(pig-pen-headless)

if(PIGPEN_BUILD_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG a74efa0d5628b74adc0426af4c5710e287fa7c2c
    GIT_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG e0b59689461f03cbdc9aa394ea1512c1f0e6e246
    GIT_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(glfw imgui)
  find_package(OpenGL REQUIRED)

  add_library(
    pigpen_imgui STATIC
      "${imgui_SOURCE_DIR}/imgui.cpp"
      "${imgui_SOURCE_DIR}/imgui_draw.cpp"
      "${imgui_SOURCE_DIR}/imgui_tables.cpp"
      "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
  )
  target_include_directories(
    pigpen_imgui
    SYSTEM PUBLIC
      "${imgui_SOURCE_DIR}"
      "${imgui_SOURCE_DIR}/backends"
  )
  target_link_libraries(pigpen_imgui PUBLIC glfw OpenGL::GL)
  add_executable(
    pig-pen
      src/app/main.cpp
      src/ui/app_ui.cpp
      src/ui/gui_options.cpp
      src/ui/world_animation.cpp
  )
  target_link_libraries(
    pig-pen
    PRIVATE pigpen_agent pigpen_imgui
  )
  pigpen_configure_target(pig-pen)
  target_include_directories(pig-pen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

if(PIGPEN_BUILD_TESTS)
  include(CTest)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG 56809e5282f104c5c8b570e7c2996cdc352d94f1
    GIT_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(Catch2)

  add_executable(
    pigpen_tests
      tests/episode_runner_tests.cpp
      tests/gui_options_tests.cpp
      tests/metrics_writer_tests.cpp
      tests/prompt_tests.cpp
      tests/session_tests.cpp
      tests/world_animation_tests.cpp
      tests/world_tests.cpp
      src/ui/gui_options.cpp
      src/ui/world_animation.cpp
  )
  target_link_libraries(
    pigpen_tests
    PRIVATE
      Catch2::Catch2WithMain
      pigpen_agent
  )
  pigpen_configure_target(pigpen_tests)
  target_include_directories(
    pigpen_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )

  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  include(Catch)
  catch_discover_tests(pigpen_tests)

  add_executable(
    pigpen_reflection_tests
      tests/world_tools_tests.cpp
  )
  target_link_libraries(
    pigpen_reflection_tests
    PRIVATE
      Catch2::Catch2WithMain
      pigpen_reflected_tools
  )
  pigpen_configure_target(pigpen_reflection_tests)
  target_include_directories(
    pigpen_reflection_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  catch_discover_tests(pigpen_reflection_tests)

  add_test(NAME pigpen_headless_help COMMAND pig-pen-headless --help)
  add_test(NAME pigpen_headless_requires_model COMMAND pig-pen-headless)
  set_tests_properties(
    pigpen_headless_requires_model
    PROPERTIES WILL_FAIL TRUE
  )
  add_test(
    NAME pigpen_headless_rejects_invalid_bounds
    COMMAND pig-pen-headless --max-tool-rounds 65
  )
  set_tests_properties(
    pigpen_headless_rejects_invalid_bounds
    PROPERTIES WILL_FAIL TRUE
  )
  add_test(
    NAME pigpen_headless_rejects_invalid_temperature
    COMMAND pig-pen-headless --temperature nan
  )
  set_tests_properties(
    pigpen_headless_rejects_invalid_temperature
    PROPERTIES WILL_FAIL TRUE
  )

  add_test(
    NAME pigpen_reflection_integration
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflection_integration_test.py"
      "$<TARGET_FILE:pig-pen-headless>"
  )
  set_tests_properties(pigpen_reflection_integration PROPERTIES TIMEOUT 45)

  if(UNIX)
    foreach(PIGPEN_SIGNAL IN ITEMS SIGINT SIGTERM)
      string(TOLOWER "${PIGPEN_SIGNAL}" PIGPEN_SIGNAL_TEST_SUFFIX)
      add_test(
        NAME "pigpen_headless_graceful_${PIGPEN_SIGNAL_TEST_SUFFIX}"
        COMMAND
          "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_signal_test.py"
          "$<TARGET_FILE:pig-pen-headless>"
          "${PIGPEN_SIGNAL}"
      )
      set_tests_properties(
        "pigpen_headless_graceful_${PIGPEN_SIGNAL_TEST_SUFFIX}"
        PROPERTIES TIMEOUT 30
      )
    endforeach()
  endif()
endif()
