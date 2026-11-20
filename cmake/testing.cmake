# Two Catch2 binaries (a C++23 one, and a reflection one that links
# scrydantic), CLI checks against the headless binary, and Python tests that
# drive the CLI against the scripted fake llamad. No model or external network
# is needed; the fake listens on a Unix socket in a temporary directory.

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
# The tests parse logs and requests independently of scrydantic's codec.
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2 nlohmann_json)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# The scripted fake llamad: a real gRPC server on a temporary Unix socket,
# behind a gRPC-free header. The C++ tests link it in process; the Python CLI
# tests run it as pigpen_fake_llamad.
add_library(pigpen_test_support STATIC tests/support/fake_llamad.cpp)
target_include_directories(pigpen_test_support PUBLIC "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(pigpen_test_support PRIVATE pigpen_llamad_proto)
pigpen_target(pigpen_test_support)

add_executable(pigpen_fake_llamad tests/support/fake_llamad_main.cpp)
target_link_libraries(
  pigpen_fake_llamad
  PRIVATE pigpen_test_support nlohmann_json::nlohmann_json
)
pigpen_target(pigpen_fake_llamad)

# C++23 test TUs. They link the reflection layer's objects (Session,
# MetricsWriter) through pigpen_agent but never include a scrydantic header.
add_executable(
  pigpen_tests
    tests/episode_runner_tests.cpp
    tests/gui_options_tests.cpp
    tests/llamad_client_tests.cpp
    tests/metrics_writer_tests.cpp
    tests/prompt_tests.cpp
    tests/session_tests.cpp
    tests/tool_loop_tests.cpp
    tests/world_animation_tests.cpp
    tests/world_tests.cpp
    tests/world_tools_tests.cpp
)
target_link_libraries(
  pigpen_tests
  PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json pigpen_agent
          pigpen_llamad pigpen_test_support pigpen_ui
)
pigpen_target(pigpen_tests)
catch_discover_tests(pigpen_tests)

# scrydantic::scrydantic carries C++26 and -freflection into these TUs.
add_executable(pigpen_reflection_tests tests/world_tool_binding_tests.cpp)
target_link_libraries(
  pigpen_reflection_tests
  PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    pigpen_agent
    scrydantic::scrydantic
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
# A socket that nothing serves must fail at startup with a clear message.
pigpen_headless_rejects(
  reports_unreachable_daemon
  "could not reach llamad at /nonexistent/pigpen-test.sock"
  --socket /nonexistent/pigpen-test.sock
)
pigpen_headless_rejects(
  rejects_invalid_bounds
  "--max-tool-rounds must be in the range"
  --max-tool-rounds 65
)
pigpen_headless_rejects(
  rejects_invalid_max_output_tokens
  "--max-output-tokens must be in the range 1..2147483647"
  --max-output-tokens 2147483648
)
pigpen_headless_rejects(
  rejects_zero_max_output_tokens
  "--max-output-tokens must be in the range 1..2147483647"
  --max-output-tokens 0
)
pigpen_headless_rejects(
  rejects_invalid_temperature
  "--temperature must be a finite number"
  --temperature nan
)
pigpen_headless_rejects(
  rejects_invalid_sampling_seed
  "--sampling-seed must be in the range"
  --sampling-seed 4294967296
)
pigpen_headless_rejects(
  rejects_invalid_stall_seconds
  "--stall-seconds must be in the range 0..86400"
  --stall-seconds 86401
)

add_test(
  NAME pigpen_headless_integration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_integration_tests.py"
    "$<TARGET_FILE:pig-pen-headless>"
    "$<TARGET_FILE:pigpen_fake_llamad>"
)
set_tests_properties(pigpen_headless_integration PROPERTIES TIMEOUT 45)

# A request that hangs, then a daemon killed with SIGKILL (exit 1), the
# --timeout-seconds limit (exit 3), or the --stall-seconds watchdog (exit 1).
# Each must still write a complete footer. A handshake that does not answer
# must end at --timeout-seconds 1 (exit 1, no log).
foreach(scenario IN ITEMS daemon-killed timeout stall connect-timeout)
  string(REPLACE "-" "_" suffix "${scenario}")
  add_test(
    NAME pigpen_headless_${suffix}
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_failure_tests.py"
      "$<TARGET_FILE:pig-pen-headless>"
      "$<TARGET_FILE:pigpen_fake_llamad>"
      ${scenario}
  )
  set_tests_properties(pigpen_headless_${suffix} PROPERTIES TIMEOUT 45)
endforeach()

if(UNIX)
  foreach(signal IN ITEMS SIGINT SIGTERM)
    string(TOLOWER "${signal}" suffix)
    add_test(
      NAME pigpen_headless_graceful_${suffix}
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_signal_tests.py"
        "$<TARGET_FILE:pig-pen-headless>"
        "$<TARGET_FILE:pigpen_fake_llamad>"
        ${signal}
    )
    set_tests_properties(
      pigpen_headless_graceful_${suffix}
      PROPERTIES TIMEOUT 45
    )
    # The signal arrives while the handshake waits, then the handshake fails:
    # exit 128 + signal, not 1.
    add_test(
      NAME pigpen_headless_handshake_${suffix}
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_failure_tests.py"
        "$<TARGET_FILE:pig-pen-headless>"
        "$<TARGET_FILE:pigpen_fake_llamad>"
        handshake-${suffix}
    )
    set_tests_properties(
      pigpen_headless_handshake_${suffix}
      PROPERTIES TIMEOUT 45
    )
  endforeach()
endif()
