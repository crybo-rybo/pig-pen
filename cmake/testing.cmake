# Two Catch2 binaries (the reflection one links scry directly), CLI checks
# against the headless and worker binaries, Python loopback tests, and the
# example rollout consumer. No model server or network needed.

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
    tests/cli_options_tests.cpp
    tests/episode_batch_tests.cpp
    tests/episode_driver_tests.cpp
    tests/episode_runner_tests.cpp
    tests/episode_summary_tests.cpp
    tests/episode_turn_tests.cpp
    tests/gui_options_tests.cpp
    tests/line_reader_tests.cpp
    tests/metrics_writer_tests.cpp
    tests/prompt_tests.cpp
    tests/reward_tests.cpp
    tests/session_tests.cpp
    tests/world_animation_tests.cpp
    tests/worker_jobs_tests.cpp
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
pigpen_headless_rejects(
  rejects_unknown_reward_weight
  "unknown reward weight \"bogus\""
  --model m --reward bogus=1
)
pigpen_headless_rejects(
  rejects_invalid_reward_value
  "reward weight invalid_call must be a finite number"
  --model m --reward=invalid_call=inf
)

add_test(
  NAME pigpen_headless_integration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_integration_tests.py"
    "$<TARGET_FILE:pig-pen-headless>"
)
set_tests_properties(pigpen_headless_integration PROPERTIES TIMEOUT 45)

add_test(NAME pigpen_worker_help COMMAND pig-pen-worker --help)

# Pass only when the worker binary prints the expected diagnostic.
function(pigpen_worker_rejects name diagnostic)
  add_test(NAME pigpen_worker_${name} COMMAND pig-pen-worker ${ARGN})
  set_tests_properties(
    pigpen_worker_${name}
    PROPERTIES PASS_REGULAR_EXPRESSION "${diagnostic}"
  )
endfunction()
pigpen_worker_rejects(requires_model "--model is required" --seeds 1)
pigpen_worker_rejects(requires_seeds "--seeds is required" --model m)
pigpen_worker_rejects(
  rejects_world_seed_flag
  "--seed is not accepted by the worker"
  --model m --seeds 1 --seed 2
)
pigpen_worker_rejects(
  rejects_malformed_seeds
  "--seeds entry \"1x\" is not a seed or an A-B range"
  --model m --seeds 1x
)
pigpen_worker_rejects(
  rejects_descending_seed_range
  "--seeds range \"9-3\" must not descend"
  --model m --seeds 9-3
)
pigpen_worker_rejects(
  rejects_duplicate_seed
  "--seeds lists seed 4 more than once"
  --model m --seeds 3-5 --seeds 4
)
pigpen_worker_rejects(
  rejects_zero_samples
  "--samples must be in the range 1..10000"
  --model m --seeds 1 --samples 0
)
pigpen_worker_rejects(
  rejects_zero_parallel
  "--parallel must be in the range 1..256"
  --model m --seeds 1 --parallel 0
)
pigpen_worker_rejects(
  rejects_sampling_seed_overflow
  "--sampling-seed-base 4294967295 leaves no room for 2 samples"
  --model m --seeds 1 --samples 2 --sampling-seed-base 4294967295
)
pigpen_worker_rejects(
  rejects_two_sampling_seeds
  "--sampling-seed and --sampling-seed-base cannot both be given"
  --model m --seeds 1 --sampling-seed 1 --sampling-seed-base 2
)
pigpen_worker_rejects(
  rejects_bad_rollout_prefix
  "--rollout-prefix must be visible ASCII without spaces"
  --model m --seeds 1 "--rollout-prefix=run 42"
)
pigpen_worker_rejects(
  rejects_reserved_header
  "--header X-Pigpen-Seed is set by the worker itself"
  --model m --seeds 1 --header X-Pigpen-Seed=1
)
pigpen_worker_rejects(
  rejects_unknown_reward_weight
  "unknown reward weight \"bogus\""
  --model m --seeds 1 --reward bogus=1
)
pigpen_worker_rejects(
  rejects_jobs_with_seeds
  "--jobs - cannot be combined with --seeds"
  --model m --jobs - --seeds 1
)
pigpen_worker_rejects(
  rejects_jobs_with_samples
  "--jobs - cannot be combined with --samples"
  --model m --jobs - --samples 2
)
pigpen_worker_rejects(
  rejects_jobs_file
  "--jobs accepts only - \\(job lines on standard input\\)"
  --model m --jobs jobs.jsonl
)

add_test(
  NAME pigpen_worker_integration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_integration_tests.py"
    "$<TARGET_FILE:pig-pen-worker>"
)
set_tests_properties(pigpen_worker_integration PROPERTIES TIMEOUT 90)

add_test(
  NAME pigpen_rollout_consumer
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/rollout_consumer_tests.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/rollout_consumer.py"
    "$<TARGET_FILE:pig-pen-worker>"
)
set_tests_properties(pigpen_rollout_consumer PROPERTIES TIMEOUT 60)

if(UNIX)
  # Raw non-UTF-8 argv bytes cannot be spelled portably, hence Python.
  foreach(flavour IN ITEMS headless worker)
    add_test(
      NAME pigpen_${flavour}_rejects_invalid_utf8
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_utf8_tests.py"
        "$<TARGET_FILE:pig-pen-${flavour}>"
        ${flavour}
    )
    set_tests_properties(
      pigpen_${flavour}_rejects_invalid_utf8
      PROPERTIES TIMEOUT 60
    )
  endforeach()

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
