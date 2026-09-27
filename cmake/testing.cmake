# Pig Pen's test suite: two Catch2 binaries (kept separate so the -freflection
# translation units stay isolated), CLI smoke tests against the headless
# binary, and the Python-driven integration tests. Needs no model server or
# network.

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
)
target_link_libraries(
  pigpen_tests
  PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    pigpen_agent
    pigpen_ui
)
pigpen_target(pigpen_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(pigpen_tests)

add_executable(pigpen_reflection_tests
  tests/world_tools_tests.cpp
  tests/scry_transport_tests.cpp
)
target_link_libraries(
  pigpen_reflection_tests
  PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    pigpen_reflected_tools
    pigpen_agent
    scry::testing
)
pigpen_target(pigpen_reflection_tests)
catch_discover_tests(pigpen_reflection_tests)

# Headless CLI behaviour. `--help` exits 0; everything registered through this
# helper must exit non-zero.
add_test(NAME pigpen_headless_help COMMAND pig-pen-headless --help)

function(pigpen_failing_headless_test name)
  add_test(NAME pigpen_headless_${name} COMMAND pig-pen-headless ${ARGN})
  set_tests_properties(pigpen_headless_${name} PROPERTIES WILL_FAIL TRUE)
endfunction()
pigpen_failing_headless_test(requires_model)
pigpen_failing_headless_test(rejects_invalid_bounds --max-tool-rounds 65)
pigpen_failing_headless_test(rejects_invalid_temperature --temperature nan)
# The sampling seed is 32 bits (Scry's SamplingConfig::seed); match the
# diagnostic so an accepted value that later fails to connect cannot pass.
add_test(
  NAME pigpen_headless_rejects_invalid_sampling_seed
  COMMAND pig-pen-headless --model m --sampling-seed 4294967296
)
set_tests_properties(
  pigpen_headless_rejects_invalid_sampling_seed
  PROPERTIES PASS_REGULAR_EXPRESSION "--sampling-seed must be in the range"
)

add_test(
  NAME pigpen_reflection_integration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflection_integration_tests.py"
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
