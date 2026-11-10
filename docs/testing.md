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
dependencies, **a model, a llamad daemon, and external network access are not
necessary**. The tests use scripted chat clients, and a scripted fake llamad.
The fake llamad is a real gRPC server on a Unix socket in a short temporary
directory (`/tmp/pigpen-XXXXXX/s.sock`). AF_UNIX paths are limited to about
107 bytes, so the tests never put a socket under `build/`. You can run the
full suite offline.

The configure step is also a check for reflection support. It makes sure
that GCC 16.2 or later and a Python 3 interpreter are available. The
configure step of scrydantic also refuses GCC versions before 16.2. Thus, an
unsupported compiler fails before compilation starts.

## Test contents

`ctest` finds two types of tests.

**Catch2 cases.** These cases come from two binaries:

- `pigpen_tests`, which compiles as C++23.
- `pigpen_reflection_tests`, which isolates reflection. It contains only
  `world_tool_binding_tests.cpp`, because that file includes the scrydantic
  toolbox.

`catch_discover_tests` registers each case separately. The cases test these
items:

| file | tests |
|---|---|
| `tests/world_tests.cpp` | Grid constants, item placement from the seed, movement, and wall failures. `look` rays, the `eat` action, and the score. The end of positive items, and seed determinism with `World::dump()`. |
| `tests/world_tools_tests.cpp` | flat typed responses, and the `opaque_look` and `reward_feedback` toggles |
| `tests/tool_loop_tests.cpp` | `ToolLoopTransport` with a scripted chat client and a fake dispatcher: the system message first, the tools on each round, the tool results with their call IDs, and the seed only when it is set. The call budget across batches and its reset for each turn, invalid calls that use the budget, admission refusals, and the round-limit completion (history kept, the count of calls that did not run, no call without an answer). The length limit as an error, an error after a tool round, a later round that cannot start, cancellation during a round, the transactional rollback, the token totals across rounds, and destruction during a request. |
| `tests/llamad_client_tests.cpp` | `LlamadChatClient` against the fake llamad: the wire mapping in both directions, the stream order, each finish reason, each failure status, cancellation, a callback that starts the next request, the bounded `pump()`, an undelivered terminal event that blocks the next request, the connect rejections (missing socket, embedding model, no chat template), and destruction with a stream in flight |
| `tests/world_tool_binding_tests.cpp` | The exact tool manifest (names, descriptions, and schema strings), the canonical encoder, decode errors and unknown tools that the model sees, the exact activity payloads and call identity, a wall as a typed outcome, and admission after objective completion and after a log failure, also in one batch through the real tool loop |
| `tests/prompt_tests.cpp` | config defaults, and that each prompt flag does what it claims. This includes hidden rewards, and the separation of automatic recovery instructions from human guidance. |
| `tests/episode_runner_tests.cpp` | the turn loop with a scripted transport: budget exhaustion, pause and resume, a stop that cancels an active turn, objective completion, terminal errors and log errors, and queued human input |
| `tests/metrics_writer_tests.cpp` | header, tool, turn, and footer reconciliation (format 2), the incomplete footer at destruction, and footer finality |
| `tests/session_tests.cpp` | config rejection, an unreachable daemon that leaves no log, and full scripted episodes through the real gRPC path with the log examined from start to end. This includes a stop during a request that hangs, and destruction during a request. |
| `tests/world_animation_tests.cpp` | how the typed activity feed becomes an ordered visual timeline, with a time that the caller supplies |
| `tests/gui_options_tests.cpp` | the GUI startup parser for `--socket`, `--play`, and `--help`, with the two value syntaxes and input that is not valid, and the llamad default socket rule |

**CLI tests.** `cmake/testing.cmake` registers these tests. The Python tests
start `pigpen_fake_llamad`, which is the same fake as a process. It reads a
JSON script of replies, and writes each request that it gets as one JSON line.
Thus, the Python tests do not need Python gRPC packages.

- `pigpen_headless_integration`: `tests/headless_integration_tests.py` runs
  the CLI against the fake llamad. It tests how the CLI reads argv (with
  `--socket` and `--sampling-seed`), the tools and the tool result that go to
  the daemon, stdout, the JSONL log on disk, and the exit codes. Exit code `0`
  is for a valid `move`, and exit code `5` is for only a schema-invalid call.
- `pigpen_headless_help`: makes sure that `--help` exits with code 0.
- `pigpen_headless_reports_unreachable_daemon`: passes only if the CLI gives
  the clear startup error for a socket that nothing serves.
- `pigpen_headless_rejects_invalid_bounds` (`--max-tool-rounds 65`),
  `pigpen_headless_rejects_invalid_temperature` (`--temperature nan`), and
  `pigpen_headless_rejects_invalid_sampling_seed` (a value above the 32-bit
  range): each test passes only if the CLI prints the correct option
  diagnostic.
- `pigpen_headless_graceful_sigint` / `_sigterm`:
  `tests/headless_signal_tests.py` does these steps:
  1. It starts the fake llamad with a reply that never finishes.
  2. It points the CLI to the fake, and waits for the first request.
  3. It sends the signal.
  4. It makes sure that the exit status is `128 + signal`, that the cancelled
     turn records its one request, that the header has the model description
     and the socket, and that the JSONL file still ends with a final footer.

When the tests are on, Python 3 is necessary. The two signal tests register
only on UNIX.

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
full path manually, start llamad with a model, run a short episode, and
examine the exit code:

```sh
llamad --model /absolute/path/to/model.gguf --ctx 32768 &
./build/dev/pig-pen-headless --turns 2 --seed 42 --timeout-seconds 120
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
