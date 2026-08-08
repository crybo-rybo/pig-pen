# Testing

```sh
just test              # build the dev preset, then run ctest
```

or, without `just`:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The suite completes in a few seconds. **No external model server or external
network access is required** after dependencies have been fetched. Unit tests
use fake transports; the public-boundary integration test runs real Scry/Curl
traffic against a loopback stub, so the suite remains safe to run offline.

Configuration itself is part of the reflection gate: it requires GCC 16+ and a
Python 3 interpreter, asks Scry to probe the P2996/P3394 facilities it uses,
and fails before compilation if that surface is unavailable.

## What runs

`ctest` picks up two kinds of test.

**Catch2 cases** from `pigpen_tests` and the reflection-isolated
`pigpen_reflection_tests`, registered individually via `catch_discover_tests`,
covering:

| file | covers |
|---|---|
| `tests/world_tests.cpp` | grid constants, seeded placement, movement and wall failures, `look` rays, eating and scoring, positive-item exhaustion, and seed determinism via `World::dump()` |
| `tests/world_tools_tests.cpp` | compile-time reflected schemas, typed result envelopes, the explicit turn/budget lifecycle, reflection-based observability projection, and the `opaque_look` / `reward_feedback` toggles |
| `tests/prompt_tests.cpp` | config defaults and that each prompt flag says what it claims — including that the hidden-values prompt never leaks the reward table |
| `tests/episode_runner_tests.cpp` | the turn loop against a scripted transport: budget exhaustion, pause/resume, stop cancelling an in-flight turn, objective completion, terminal errors, and queued human input |
| `tests/metrics_writer_tests.cpp` | header/tool/turn/footer reconciliation, the incomplete footer on destruction, and footer finality |
| `tests/session_tests.cpp` | config rejection and that a session owns a seeded world plus a registered tool harness atomically |
| `tests/world_animation_tests.cpp` | the event feed becoming an ordered visual timeline, with caller-supplied time |
| `tests/gui_options_tests.cpp` | GUI startup parsing for model and endpoint arguments, including both value syntaxes and invalid input |

**CLI tests** registered directly in `CMakeLists.txt`:

- `pigpen_reflection_integration` — a loopback OpenAI-compatible server verifies
  the provider-visible reflected schemas, strict typed decode, encoded response
  envelope, world event, and JSONL record through the real Scry/Curl path
- `pigpen_headless_help` — `--help` exits 0
- `pigpen_headless_requires_model` — omitting `--model` must fail
- `pigpen_headless_rejects_invalid_bounds` — `--max-tool-rounds 65` must fail
- `pigpen_headless_rejects_invalid_temperature` — non-finite sampling values
  must fail
- `pigpen_headless_graceful_sigint` / `_sigterm` — `tests/headless_signal_test.py`
  starts a stub socket server on a loopback port, points the CLI at it, sends
  an exact tagged model identifier, verifies that identifier in the HTTP
  request and JSONL header, then asserts the exit status is `128 + signal`
  *and* that the JSONL file still ends with a finalized footer

Python 3 is required whenever tests are enabled. The two signal tests only
register on UNIX; the loopback reflection integration test runs on every
supported platform.

## Running a subset

```sh
ctest --preset dev -R world              # by test name
ctest --preset dev --output-on-failure   # already the preset default
./build/dev/pigpen_tests --list-tests
./build/dev/pigpen_tests "[determinism]" # Catch2 tags
./build/dev/pigpen_tests -s "eating consumes each item and applies its reward"
./build/dev/pigpen_reflection_tests --list-tests
```

## Testing against a real model

The suite deliberately never contacts one. To exercise the full path by hand,
run a short episode and check the exit code:

```sh
./build/dev/pig-pen-headless --model YOUR_MODEL --turns 2 --seed 42 --timeout-seconds 120
echo $?
```

Exit `0` means the episode finished and produced at least one successfully
decoded world-tool invocation. Exit `5` means it produced none, including a
turn containing only schema-invalid calls. See
[Running](running.md#exit-codes) for the rest, and [Logs](logs.md) for
inspecting what happened.

Note that `pig-pen-headless` writes to `logs/` in the current working
directory — `cd` to a scratch directory first if you would rather not add to
the project's logs.
