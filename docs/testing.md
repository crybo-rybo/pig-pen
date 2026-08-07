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

44 tests, a few seconds end to end. **No model server and no network access are
required** — the transport is faked everywhere, so the suite is safe to run in
CI or offline.

## What runs

`ctest` picks up two kinds of test.

**Catch2 cases (40)** from `pigpen_tests`, registered individually via
`catch_discover_tests`, covering:

| file | covers |
|---|---|
| `tests/world_tests.cpp` | grid constants, seeded placement, movement and wall failures, `look` rays, eating and scoring, positive-item exhaustion, and seed determinism via `World::dump()` |
| `tests/tool_executor_tests.cpp` | the published tool schemas, argument validation, result shapes, the `opaque_look` and `reward_feedback` toggles, and the one-event-per-call invariant |
| `tests/prompt_tests.cpp` | config defaults and that each prompt flag says what it claims — including that the hidden-values prompt never leaks the reward table |
| `tests/episode_runner_tests.cpp` | the turn loop against a scripted transport: budget exhaustion, pause/resume, stop cancelling an in-flight turn, objective completion, terminal errors, and queued human input |
| `tests/metrics_writer_tests.cpp` | header/tool/turn/footer reconciliation, the incomplete footer on destruction, and footer finality |
| `tests/session_tests.cpp` | config rejection and that a session owns a seeded world plus a registered tool harness atomically |
| `tests/world_animation_tests.cpp` | the event feed becoming an ordered visual timeline, with caller-supplied time |

**CLI tests (4)** registered directly in `CMakeLists.txt`:

- `pigpen_headless_help` — `--help` exits 0
- `pigpen_headless_rejects_invalid_bounds` — `--max-tool-rounds 65` must fail
- `pigpen_headless_graceful_sigint` / `_sigterm` — `tests/headless_signal_test.py`
  starts a stub socket server on a loopback port, points the CLI at it, sends
  the signal, and asserts the exit status is `128 + signal` *and* that the JSONL
  file still ends with a finalized footer

The two signal tests need a Python 3 interpreter and only register on UNIX. If
CMake does not find one they are silently skipped; the rest of the suite is
unaffected.

## Running a subset

```sh
ctest --preset dev -R world              # by test name
ctest --preset dev --output-on-failure   # already the preset default
./build/dev/pigpen_tests --list-tests
./build/dev/pigpen_tests "[determinism]" # Catch2 tags
./build/dev/pigpen_tests -s "eating consumes each item and applies its reward"
```

## Testing against a real model

The suite deliberately never contacts one. To exercise the full path by hand,
run a short episode and check the exit code:

```sh
./build/dev/pig-pen-headless --model qwen3:0.6b --turns 2 --seed 42 --timeout-seconds 120
echo $?
```

Exit `0` means the episode finished *and* the model called at least one tool;
exit `5` means it talked without ever touching the world. See
[Running](running.md#exit-codes) for the rest, and [Logs](logs.md) for
inspecting what happened.

Note that `pig-pen-headless` writes to `logs/` in the current working
directory — `cd` to a scratch directory first if you would rather not add to
the project's logs.
