# Test Pig Pen

```sh
just test              # build the dev preset, then run ctest
just ci                # format + lint + dev/release/headless (the GitHub PR checks)
```

If you do not use `just`, use these commands:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The suite completes in a few seconds. After the configure step gets the
dependencies, **an external model server and external network access are not
necessary**. The unit tests use fake transports and the public scripted
transport from Scry. The CLI integration test sends real Scry and curl traffic
to a loopback stub. You can run the full suite offline.

The configure step is also a check for reflection support. It does these
steps:

1. It makes sure that GCC 16 or later and a Python 3 interpreter are
   available.
2. It asks Scry to probe for the P2996/P3394 features that Pig Pen uses.
3. If these features are not available, it fails before compilation starts.

## Test contents

`ctest` finds two types of tests.

**Catch2 cases.** These cases come from two binaries:

- `pigpen_tests`, which compiles as C++23.
- `pigpen_reflection_tests`, which isolates reflection. It contains only
  `scry_transport_tests.cpp`, because that file includes the Scry headers.

`catch_discover_tests` registers each case separately. The cases test these
items:

| file | tests |
|---|---|
| `tests/world_tests.cpp` | Grid constants, item placement from the seed, movement, and wall failures. `look` rays, the `eat` action, and the score. The end of positive items, and seed determinism with `World::dump()`. |
| `tests/world_tools_tests.cpp` | flat typed responses, and the `opaque_look` and `reward_feedback` toggles |
| `tests/scry_transport_tests.cpp` | These tests use `scry::testing`. They test compile-time reflected schemas, the public Scry encoder, and standalone registry manifests. They test native call budgets across batches and turns, and decode errors that the model sees. They test exact dispatch payloads and identity, and side effects of a dispatch failure and of shutdown. They also test admission after objective completion and log failure, history preservation at the round limit, cancellation, and transport lifetime. |
| `tests/prompt_tests.cpp` | config defaults, and that each prompt flag does what it claims. This includes hidden rewards, and the separation of automatic recovery instructions from human guidance. |
| `tests/episode_runner_tests.cpp` | the turn loop with a scripted transport: budget exhaustion, pause and resume, a stop that cancels an active turn, objective completion, terminal errors and log errors, and queued human input |
| `tests/metrics_writer_tests.cpp` | header, tool, turn, and footer reconciliation, the incomplete footer at destruction, and footer finality |
| `tests/session_tests.cpp` | config rejection, and that a session atomically owns a seeded world and a registered tool harness |
| `tests/world_animation_tests.cpp` | how the typed activity feed becomes an ordered visual timeline, with a time that the caller supplies |
| `tests/gui_options_tests.cpp` | GUI startup parser for model and endpoint arguments. This includes the two value syntaxes and input that is not valid. |

**CLI tests.** `cmake/testing.cmake` registers these tests:

- `pigpen_headless_integration`: `tests/headless_integration_tests.py` runs
  the CLI against a loopback OpenAI-compatible stub, through the real Scry and
  curl path. It tests how the CLI reads argv (with `--sampling-seed`), and
  the tool result that goes back to the provider. It also tests stdout, the
  JSONL log on disk, and the exit codes. Exit code `0` is for a valid `move`, and exit code `5` is for
  only a schema-invalid call.
- `pigpen_headless_help`: makes sure that `--help` exits with code 0.
- `pigpen_headless_requires_model`, `pigpen_headless_rejects_invalid_bounds`
  (`--max-tool-rounds 65`), `pigpen_headless_rejects_invalid_temperature`
  (`--temperature nan`), and `pigpen_headless_rejects_invalid_sampling_seed`
  (a value above the 32-bit range): each test passes only if the CLI prints
  the correct option diagnostic.
- `pigpen_headless_graceful_sigint` / `_sigterm`:
  `tests/headless_signal_tests.py` does these steps:
  1. It starts a stub socket server on a loopback port.
  2. It points the CLI to the server, and sends an exact tagged model
     identifier.
  3. It makes sure that the HTTP request and the JSONL header have that
     identifier.
  4. It makes sure that the exit status is `128 + signal`, *and* that the
     JSONL file still ends with a final footer.

When the tests are on, Python 3 is necessary. The two signal tests register
only on UNIX. The loopback integration test runs on all supported platforms.

## Run some of the tests

```sh
ctest --preset dev -R world              # by test name
./build/dev/pigpen_tests --list-tests
./build/dev/pigpen_tests "[determinism]" # Catch2 tags
./build/dev/pigpen_tests -s "eating consumes each item and applies its reward"
./build/dev/pigpen_reflection_tests --list-tests
```

## Test with a real model

The suite never connects to a real model. This is intentional. To test the
full path manually, run a short episode and examine the exit code:

```sh
./build/dev/pig-pen-headless --model YOUR_MODEL --turns 2 --seed 42 --timeout-seconds 120
echo $?
```

Exit code `0` shows that the episode finished with at least one decoded
world-tool call. Exit code `5` shows that the episode had no decoded world-tool
calls. For example, this occurs when a turn has only schema-invalid calls. For
the other exit codes, refer to [Run Pig Pen](running.md#exit-codes). To
examine the events of the run, refer to [Logs](logs.md).

NOTE: `pig-pen-headless` writes to `logs/` in the current working directory.
If you do not want to add files to the project logs, go (`cd`) to a scratch
directory before you start the CLI.

`SCRY_BUILD_TESTING_SUPPORT` has the same value as `PIGPEN_BUILD_TESTS`. The
build links the scripted component only into the reflection test binary.
Production builds do not include it. The loopback tests are still necessary to
test the CLI startup, curl, and the OS signals.
