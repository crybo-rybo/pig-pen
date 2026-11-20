# Test Pig Pen

```sh
just test              # build the dev preset, then run ctest
just ci                # format + lint + dev/release/headless/asan (the GitHub PR checks)
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
| `tests/tool_loop_tests.cpp` | `ToolLoopTransport` with a scripted chat client and a fake dispatcher: the system message first, the tools on each round, the tool results with their call IDs, and the seed only when it is set. The call budget across batches and its reset for each turn, invalid calls that use the budget, admission refusals, and the round-limit completion (history kept, the count of calls that did not run, no call without an answer). An empty final reply as an empty assistant message, and the roles of the next request. The committed history after an error, a length limit, a full context, a cancellation, or a later round that cannot start: the answered rounds stay, and a turn without an answered round adds nothing. A `length` finish at a full context (`context_exhausted`), below the context (an error that names the output limit), and with an unknown context (an error that names both causes). The kind of a client error: `failure` is an error, `cancelled` is cancelled, and `prompt_too_long` is `context_exhausted` with the daemon's message, unless a cancel came first. A cancel during a request, during a tool call, and before an error or a final reply. The token totals and the daemon timings across rounds, reasoning off on each request, and destruction during a request. |
| `tests/llamad_client_tests.cpp` | `LlamadChatClient` against the fake llamad: the wire mapping in both directions (with the reasoning field, the timings, and the model fields), the stream order, progress chunks that the client skips, each finish reason, each failure status, `OUT_OF_RANGE` as `prompt_too_long` with the daemon's message, a daemon that stops during a stream (an error, not a cancellation), cancellation, a cancel immediately after `start()` (before the request gets to the daemon), a cancel after the stream ended but before `pump()` delivers the result (the daemon's result, not a cancellation, and the next request is not affected), a callback that starts the next request, the bounded `pump()`, an undelivered terminal event that blocks the next request, the connect rejections (missing socket, a daemon that is not llamad 0.2.x, embedding model, no chat template), and destruction with a stream in flight. Each cancel test makes sure that exactly one `on_finished` arrives. |
| `tests/world_tool_binding_tests.cpp` | The exact tool manifest (names, descriptions, and schema strings), the canonical encoder, decode errors and unknown tools that the model sees, the exact activity payloads and call identity, a wall as a typed outcome, and admission after objective completion and after a log failure, also in one batch through the real tool loop |
| `tests/prompt_tests.cpp` | config defaults, and that each prompt flag does what it claims. This includes hidden rewards, and the separation of automatic recovery instructions from human guidance. |
| `tests/episode_runner_tests.cpp` | the turn loop with a scripted transport: budget exhaustion, pause and resume, a stop that cancels an active turn, objective completion, terminal errors and log errors, a `context_exhausted` turn that ends the episode with that reason and its error text, and queued human input |
| `tests/metrics_writer_tests.cpp` | header, tool, turn, and footer reconciliation (format 2), the incomplete footer at destruction, and footer finality |
| `tests/session_tests.cpp` | config rejection, an unreachable daemon and a daemon that is not llamad 0.2.x that leave no log, and full scripted episodes through the real gRPC path with the log examined from start to end. This includes a stop during a request that hangs, and destruction during a request. A full context (`OUT_OF_RANGE`, and a `length` finish that fills `n_ctx`) after an answered round, with the configured `max_output_tokens` on the wire and the turn status and the footer in the log. The startup warning for a context below the estimate, and no warning at or above it, or without a context size. |
| `tests/world_animation_tests.cpp` | how the typed activity feed becomes an ordered visual timeline, with a time that the caller supplies |
| `tests/gui_options_tests.cpp` | the GUI startup parser for `--socket`, `--play`, and `--help`, with the two value syntaxes and input that is not valid, and the llamad default socket rule |

**CLI tests.** `cmake/testing.cmake` registers these tests. The Python tests
start `pigpen_fake_llamad`, which is the same fake as a process. It reads a
JSON script of replies, and writes each request that it gets as one JSON line.
Thus, the Python tests do not need Python gRPC packages.

- `pigpen_headless_integration`: `tests/headless_integration_tests.py` runs
  the CLI against the fake llamad. It tests how the CLI reads argv (with
  `--socket` and `--sampling-seed`), the tools, the tool result, and the
  reasoning setting that go to the daemon, stdout, the JSONL log on disk
  (with the daemon timings), and the exit codes. Exit code `0`
  is for a valid `move`, and exit code `5` is for only a schema-invalid call.
  Exit code `6` is for a full context, with the stderr line, the turn status,
  and the footer. One run is an `OUT_OF_RANGE` refusal with a small `n_ctx`,
  and the startup warning on stderr. The other run is a `length` finish that
  fills the context, with `--max-output-tokens 333` in each request.
- `pigpen_headless_help`: makes sure that `--help` exits with code 0.
- `pigpen_headless_reports_unreachable_daemon`: passes only if the CLI gives
  the clear startup error for a socket that nothing serves.
- `pigpen_headless_rejects_invalid_bounds` (`--max-tool-rounds 65`),
  `pigpen_headless_rejects_invalid_max_output_tokens`
  (`--max-output-tokens 2147483648`),
  `pigpen_headless_rejects_zero_max_output_tokens` (`--max-output-tokens 0`),
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

- `pigpen_headless_daemon_killed` / `_timeout`:
  `tests/headless_failure_tests.py` starts the fake llamad with a reply that
  never finishes, and waits for the first request of the CLI. Then:
  - `daemon_killed` stops the fake with `SIGKILL`. The CLI must exit with
    code `1`. The log must have one `turn` record with the status `error` and
    an error that starts with `llamad `, and a footer with `complete: true`
    and `finish_reason: "error"`. The test does not examine the gRPC status
    name or message, because they are different in different gRPC versions.
  - `timeout` uses `--timeout-seconds 1`. The CLI must exit with code `3`
    and write `timeout after 1 seconds` to stderr. The log must have one
    `cancelled` turn, and a footer with `complete: true` and
    `finish_reason: "stopped"`.

  If an assertion fails, the test kills and reaps the CLI and the fake, and
  the failure message includes stdout and stderr of the CLI.

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

## Run the suite with the sanitizers

The `asan` preset is a Debug build without the GUI. It adds
AddressSanitizer and UndefinedBehaviorSanitizer to the compile step and the
link step of all code that the build compiles: Pig Pen and the fetched
sources (`-fsanitize=address,undefined -fno-sanitize-recover=undefined
-fno-omit-frame-pointer`). Warnings are errors, as in the other presets. The
CI runs the preset as one more entry of the `build-and-test` matrix, and
`just ci` runs it too.

```sh
just test asan
```

If you do not use `just`, use these commands. On macOS, add `CXX=g++-16` to
the configure step.

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

The test preset sets `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, so
the first undefined behavior stops the test with a stack trace. An
AddressSanitizer report always stops the test. To run a test binary directly,
set the same variables:

```sh
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 \
  ./build/asan/pigpen_tests "[determinism]"
```

Know these limits:

- The sanitizers do not instrument gRPC and Protobuf, because the build
  does not compile them. On Linux they are system packages. On macOS, they
  are the `build/deps/grpc` build. Thus, the sanitizers find errors in Pig
  Pen code and in the fetched sources, but not in gRPC.
- Leak detection is off (`ASAN_OPTIONS=detect_leaks=0`). GCC's
  LeakSanitizer does not run on macOS. On Linux, nobody has examined what it
  reports for the system gRPC.
- ThreadSanitizer is not part of the preset. GCC's ThreadSanitizer runtime
  does not link on macOS. On Linux, the system gRPC is not instrumented, so
  ThreadSanitizer cannot see its synchronization, and its reports would not
  be reliable.

## Test with a real model

The suite never connects to a real model. This is intentional. To test the
full path manually, start llamad with a model in a second terminal:

```sh
llamad --model /absolute/path/to/model.gguf --ctx 32768
```

Wait for the line `[llamad] listening on unix:...`. llamad prints it after
the model loads. The CLI does not wait for the daemon. If you start the CLI
before this line, it fails with `could not reach llamad`. Then run a short
episode in the first terminal, and examine the exit code:

```sh
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
