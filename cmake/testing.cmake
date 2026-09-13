# Pig Pen's test suite: isolated ordinary/reflection Catch2 binaries, an
# optional ImGui layout binary, and Python-driven CLI/integration tests.
# Needs no display, model server, or external network.

include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

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
    tests/activity_history_tests.cpp
    tests/episode_runner_tests.cpp
    tests/gui_options_tests.cpp
    tests/headless_options_tests.cpp
    tests/metrics_writer_tests.cpp
    tests/prompt_tests.cpp
    tests/session_tests.cpp
    tests/text_catalog_tests.cpp
    tests/world_animation_tests.cpp
    tests/world_tests.cpp
)
target_link_libraries(
  pigpen_tests
  PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    Threads::Threads
    pigpen_agent
    pigpen_cli
    pigpen_ui
)
pigpen_target(pigpen_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(pigpen_tests)

add_executable(pigpen_reflection_tests tests/world_tools_tests.cpp)
target_link_libraries(
  pigpen_reflection_tests
  PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    pigpen_reflected_tools
)
pigpen_target(pigpen_reflection_tests)
catch_discover_tests(pigpen_reflection_tests)

if(PIGPEN_BUILD_GUI)
  # Exercise saved ImGui layouts without a display or renderer backend.
  add_executable(
    pigpen_gui_tests
      tests/app_ui_layout_tests.cpp
      src/ui/app_ui.cpp
  )
  target_link_libraries(
    pigpen_gui_tests
    PRIVATE Catch2::Catch2WithMain pigpen_agent pigpen_imgui pigpen_ui
  )
  pigpen_target(pigpen_gui_tests)
  catch_discover_tests(pigpen_gui_tests)
endif()

# Headless CLI behaviour: assert the exit code and diagnostic, so unrelated
# startup failures cannot satisfy argument-validation tests.
add_test(NAME pigpen_headless_help COMMAND pig-pen-headless --help)
add_test(
  NAME pigpen_headless_cli
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_cli_tests.py"
    "$<TARGET_FILE:pig-pen-headless>"
)
set_tests_properties(pigpen_headless_cli PROPERTIES TIMEOUT 30)

add_test(
  NAME pigpen_reflection_integration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflection_integration_tests.py"
    "$<TARGET_FILE:pig-pen-headless>"
    "${PIGPEN_RESOURCE_MANIFEST}"
)
set_tests_properties(pigpen_reflection_integration PROPERTIES TIMEOUT 45)

if(UNIX)
  foreach(PIGPEN_SIGNAL IN ITEMS SIGINT SIGTERM)
    string(TOLOWER "${PIGPEN_SIGNAL}" PIGPEN_SIGNAL_TEST_SUFFIX)
    add_test(
      NAME "pigpen_headless_graceful_${PIGPEN_SIGNAL_TEST_SUFFIX}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_signal_tests.py"
        "$<TARGET_FILE:pig-pen-headless>"
        "${PIGPEN_SIGNAL}"
    )
    set_tests_properties(
      "pigpen_headless_graceful_${PIGPEN_SIGNAL_TEST_SUFFIX}"
      PROPERTIES TIMEOUT 30
    )
  endforeach()
endif()
