cmake_minimum_required(VERSION 3.31)

project(
  pig-pen
  VERSION 0.2.0
  DESCRIPTION "A small observable world for a locally hosted LLM"
  LANGUAGES CXX
)

# C++26 reflection (P2996/P3394) is Pig Pen's model-facing type system, through
# scrydantic. Scrydantic supports GCC 16.2 or newer and refuses older ones.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.2)
  message(
    FATAL_ERROR
    "Pig Pen requires GCC 16.2 or newer with C++26 reflection "
    "(found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})"
  )
endif()

# Pig Pen uses no C++ modules; skip the per-TU module dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(PIGPEN_BUILD_GUI "Build the Dear ImGui application" ON)
option(PIGPEN_BUILD_TESTS "Build pig-pen's test suite" ON)
option(PIGPEN_WARNINGS_AS_ERRORS "Treat warnings in pig-pen code as errors" ON)
include(cmake/dependencies.cmake)

# Standard, include root, and warnings for a pig-pen target. Fetched and
# generated code never gets these warnings. Everything is C++23 except the
# reflection layer, which asks for C++26 privately so the standard never leaks
# into the targets that link it.
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

# The runtime that needs neither reflection nor gRPC: the episode state
# machine, the tool loop, prompts, and world actions. It links neither
# scrydantic nor gRPC, so a <scrydantic/...> or <grpcpp/...> include here
# cannot compile.
add_library(pigpen_core STATIC
  src/core/config.cpp
  src/core/episode_runner.cpp
  src/core/prompt.cpp
  src/core/tool_loop.cpp
  src/core/world_tools.cpp
)
target_link_libraries(pigpen_core PUBLIC pigpen_world)
pigpen_target(pigpen_core)

# Generated llamad messages and gRPC stubs. The generated headers are SYSTEM
# includes, and the generated sources do not get Pig Pen's warning flags.
set(PIGPEN_LLAMAD_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/llamad_proto")
file(MAKE_DIRECTORY "${PIGPEN_LLAMAD_GENERATED}")
get_filename_component(PIGPEN_LLAMAD_PROTO_ROOT "${PIGPEN_LLAMAD_PROTO}" DIRECTORY)
get_filename_component(PIGPEN_LLAMAD_PROTO_ROOT "${PIGPEN_LLAMAD_PROTO_ROOT}/../.." ABSOLUTE)
add_library(pigpen_llamad_proto STATIC "${PIGPEN_LLAMAD_PROTO}")
target_compile_features(pigpen_llamad_proto PUBLIC cxx_std_17)
target_link_libraries(
  pigpen_llamad_proto
  PUBLIC protobuf::libprotobuf gRPC::grpc++
)
target_include_directories(
  pigpen_llamad_proto
  SYSTEM PUBLIC "${PIGPEN_LLAMAD_GENERATED}"
)
protobuf_generate(
  TARGET pigpen_llamad_proto
  IMPORT_DIRS "${PIGPEN_LLAMAD_PROTO_ROOT}"
  PROTOC_OUT_DIR "${PIGPEN_LLAMAD_GENERATED}"
)
protobuf_generate(
  TARGET pigpen_llamad_proto
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  IMPORT_DIRS "${PIGPEN_LLAMAD_PROTO_ROOT}"
  PROTOC_OUT_DIR "${PIGPEN_LLAMAD_GENERATED}"
)

# The llamad client: core::IChatClient over the generated gRPC stubs. Its
# public header is gRPC-free; the stubs and gRPC stay PRIVATE.
add_library(pigpen_llamad STATIC src/llamad/llamad_client.cpp)
target_link_libraries(
  pigpen_llamad
  PUBLIC pigpen_core
  PRIVATE pigpen_llamad_proto
)
pigpen_target(pigpen_llamad)

# The reflection layer: the tool contract and toolbox, the reflected JSONL
# writer, and the Session that composes everything. Every TU here compiles as
# C++26 with -freflection (carried by scrydantic::scrydantic). Scrydantic and
# the C++26 requirement stay PRIVATE, so everything that links this library,
# including both front ends, stays C++23 and reflection-free.
add_library(pigpen_agent STATIC
  src/agent/metrics_writer.cpp
  src/agent/session.cpp
  src/agent/world_tool_binding.cpp
)
target_link_libraries(
  pigpen_agent
  PUBLIC pigpen_core
  PRIVATE pigpen_llamad scrydantic::scrydantic
)
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
