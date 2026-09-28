# Two Catch2 binaries (the reflection one links scry directly), CLI checks
# against the headless binary, and Python loopback tests. No model server or
# network needed.

include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG 56809e5282f104c5c8b570e7c2996cdc352d94f1
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(
  pigpen_tests
    tests/episode_runner_tests.cpp
    tests/episode_summary_tests.cpp
    tests/episode_turn_tests.cpp
    tests/gui_options_tests.cpp
    tests/metrics_writer_tests.cpp
    tests/prompt_tests.cpp
    tests/reward_tests.cpp
    tests/session_tests.cpp
    tests/world_animation_tests.cpp
    tests/world_tests.cpp
)
target_link_libraries(
  pigpen_tests
  PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json pigpen_ui
)
pigpen_target(pigpen_tests)
catch_discover_tests(pigpen_tests)

# scry::scry carries -freflection into these TUs.
add_executable(
  pigpen_reflection_tests
    tests/world_tools_tests.cpp
    tests/scry_transport_tests.cpp
)
target_link_libraries(
  pigpen_reflection_tests
  PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    pigpen_agent
    scry::scry
    scry::testing
)
pigpen_target(pigpen_reflection_tests)
catch_discover_tests(pigpen_reflection_tests)

add_test(NAME pigpen_headless_help COMMAND pig-pen-headless --help)

# Pass only when the headless binary prints the expected diagnostic.
function(pigpen_headless_rejects name diagnostic)
  add_test(NAME pigpen_headless_${name} COMMAND pig-pen-headless ${ARGN})
  set_tests_properties(
    pigpen_headless_${name}
    PROPERTIES PASS_REGULAR_EXPRESSION "${diagnostic}"
  )
endfunction()
pigpen_headless_rejects(requires_model "--model is required")
pigpen_headless_rejects(
  rejects_invalid_bounds
  "--max-tool-rounds must be in the range"
  --model m --max-tool-rounds 65
)
pigpen_headless_rejects(
  rejects_invalid_temperature
  "--temperature must be a finite number"
  --model m --temperature nan
)
pigpen_headless_rejects(
  rejects_invalid_sampling_seed
  "--sampling-seed must be in the range"
  --model m --sampling-seed 4294967296
)

add_test(
  NAME pigpen_headless_integration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_integration_tests.py"
    "$<TARGET_FILE:pig-pen-headless>"
)
set_tests_properties(pigpen_headless_integration PROPERTIES TIMEOUT 45)

if(UNIX)
  foreach(signal IN ITEMS SIGINT SIGTERM)
    string(TOLOWER "${signal}" suffix)
    add_test(
      NAME pigpen_headless_graceful_${suffix}
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_signal_tests.py"
        "$<TARGET_FILE:pig-pen-headless>"
        ${signal}
    )
    set_tests_properties(
      pigpen_headless_graceful_${suffix}
      PROPERTIES TIMEOUT 30
    )
  endforeach()
endif()
