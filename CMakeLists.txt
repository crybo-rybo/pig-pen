cmake_minimum_required(VERSION 3.25)

project(
  pig-pen
  VERSION 0.1.0
  DESCRIPTION "A small observable world for a locally hosted LLM"
  LANGUAGES CXX
)

include(FetchContent)

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
target_compile_features(pigpen_project_options INTERFACE cxx_std_23)
target_compile_options(
  pigpen_project_options
  INTERFACE
    "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>"
    # Catch2's registration macros expand __COUNTER__. Clang 22 classifies that
    # long-standing extension under -Wpedantic, so retain the strict warning
    # set while allowing the framework's macro implementation.
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-c2y-extensions>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->"
)
if(PIGPEN_WARNINGS_AS_ERRORS)
  target_compile_options(
    pigpen_project_options
    INTERFACE
      "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror>"
      "$<$<CXX_COMPILER_ID:MSVC>:/WX>"
  )
endif()

# Keep all upstream projects out of the default test/example graph. Warnings are
# still enabled for pig-pen through pigpen_project_options, but are not imposed
# on pinned third-party code.
set(SCRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_FORMAT_CHECK OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(SCRY_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_REFLECTION OFF CACHE BOOL "" FORCE)
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

set(PIGPEN_WORLD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/world/world.cpp")
if(EXISTS "${PIGPEN_WORLD_SOURCE}")
  add_library(pigpen_world STATIC "${PIGPEN_WORLD_SOURCE}")
  target_link_libraries(pigpen_world PRIVATE pigpen_project_options)
  target_include_directories(
    pigpen_world
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
  set_target_properties(
    pigpen_world
    PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON
  )
else()
  # This branch keeps configure usable while the test-driven source skeleton is
  # being added. Reconfiguring after world.cpp appears creates the real library.
  add_library(pigpen_world INTERFACE)
  target_link_libraries(pigpen_world INTERFACE pigpen_project_options)
  target_include_directories(
    pigpen_world
    INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
endif()

file(
  GLOB PIGPEN_AGENT_SOURCES
  CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/agent/*.cpp"
)
if(PIGPEN_AGENT_SOURCES)
  add_library(pigpen_agent STATIC ${PIGPEN_AGENT_SOURCES})
  target_link_libraries(
    pigpen_agent
    PUBLIC
      pigpen_world
      nlohmann_json::nlohmann_json
      scry::scry
    PRIVATE pigpen_project_options
  )
  target_include_directories(
    pigpen_agent
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
  set_target_properties(
    pigpen_agent
    PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON
  )
else()
  add_library(pigpen_agent INTERFACE)
  target_link_libraries(
    pigpen_agent
    INTERFACE
      pigpen_world
      nlohmann_json::nlohmann_json
      scry::scry
  )
  target_include_directories(
    pigpen_agent
    INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
endif()

set(PIGPEN_HEADLESS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/headless_main.cpp")
if(EXISTS "${PIGPEN_HEADLESS_SOURCE}")
  add_executable(pig-pen-headless "${PIGPEN_HEADLESS_SOURCE}")
  target_link_libraries(
    pig-pen-headless
    PRIVATE pigpen_agent pigpen_project_options
  )
  set_target_properties(
    pig-pen-headless
    PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON
  )
else()
  add_custom_target(
    pig-pen-headless
    COMMAND
      "${CMAKE_COMMAND}" -E echo
      "pig-pen-headless will be compiled after src/app/headless_main.cpp is added"
    VERBATIM
  )
endif()

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

  set(PIGPEN_GUI_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
  file(
    GLOB PIGPEN_UI_SOURCES
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
  )
  if(EXISTS "${PIGPEN_GUI_MAIN}")
    add_executable(pig-pen "${PIGPEN_GUI_MAIN}" ${PIGPEN_UI_SOURCES})
    target_link_libraries(
      pig-pen
      PRIVATE pigpen_agent pigpen_imgui pigpen_project_options
    )
    target_include_directories(
      pig-pen
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    set_target_properties(
      pig-pen
      PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON
    )
  else()
    add_custom_target(
      pig-pen
      COMMAND
        "${CMAKE_COMMAND}" -E echo
        "pig-pen will be compiled after src/app/main.cpp is added"
      VERBATIM
    )
  endif()
endif()

if(PIGPEN_BUILD_TESTS)
  include(CTest)
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

  file(
    GLOB PIGPEN_TEST_SOURCES
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  )
  if(PIGPEN_TEST_SOURCES)
    add_executable(pigpen_tests ${PIGPEN_TEST_SOURCES})
    target_sources(
      pigpen_tests
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/world_animation.cpp"
    )
    target_link_libraries(
      pigpen_tests
      PRIVATE
        Catch2::Catch2WithMain
        pigpen_agent
        pigpen_project_options
    )
    target_include_directories(
      pigpen_tests
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    set_target_properties(
      pigpen_tests
      PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON
    )

    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(pigpen_tests)
  endif()

  if(TARGET pig-pen-headless)
    add_test(NAME pigpen_headless_help COMMAND pig-pen-headless --help)
    add_test(
      NAME pigpen_headless_rejects_invalid_bounds
      COMMAND pig-pen-headless --max-tool-rounds 65
    )
    set_tests_properties(
      pigpen_headless_rejects_invalid_bounds
      PROPERTIES WILL_FAIL TRUE
    )

    if(UNIX)
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
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
  endif()
endif()
