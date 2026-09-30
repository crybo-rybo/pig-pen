# Testing

```sh
just test              # build the dev preset, then run ctest
just ci                # format + lint + dev/release/headless — what GitHub runs on a PR
```

or, without `just`:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The suite completes in a few seconds. **No external model server or external
network access is required** after dependencies have been fetched. Unit tests
use fake transports and Scry's public scripted transport; the CLI and worker integration tests run real Scry/Curl
traffic against loopback stubs, so the suite remains safe to run offline.

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
| `tests/world_tests.cpp` | grid constants, seeded placement, movement and wall failures, `look` rays, the observed-cell count, eating and scoring, positive-item exhaustion, and seed determinism via `World::dump()` |
| `tests/world_tools_tests.cpp` | compile-time reflected schemas, flat typed responses, Scry's public encoder, and the `opaque_look` / `reward_feedback` toggles |
| `tests/scry_transport_tests.cpp` | `scry_config()` forwarding the credential, request headers, and `X-Pigpen-Rollout` onto every provider request, standalone registry manifests, native call budgets across batches and turns, model-visible decode errors, exact dispatch payloads and identity, side effects on dispatch failure and shutdown, objective/logging admission, per-turn call tallies for batches mixing executed, invalid, over-budget, and post-objective requests, round-limit history preservation, cancellation, and transport lifetime using `scry::testing` |
| `tests/prompt_tests.cpp` | config defaults and that each prompt flag says what it claims — including hidden rewards and keeping automatic recovery instructions separate from human guidance |
| `tests/episode_turn_tests.cpp` | the call tally arithmetic on plain records, its absence without Scry statistics, and its refusal to fabricate a tally from inconsistent counts |
| `tests/reward_tests.cpp` | the default weights, each reward term in isolation, the total as the sum of terms, validity for every finish reason (and an unfinished episode), and `NAME=VALUE` weight parsing with each error |
| `tests/episode_summary_tests.cpp` | reward facts and summaries built from a plain world, activity feed, turns, and snapshot (no `Session`), the summary's single JSON serialisation with valid, invalid, and unfinished rewards, the worker's `episode` record (exactly the summary's line plus seed, sample, sampling seed, and `config`), `batch` record (including `job_errors`), and `job_error` record, and that no record throws on text that is not UTF-8 (it is written as U+FFFD) |
| `tests/episode_runner_tests.cpp` | the turn loop against a scripted transport: budget exhaustion, pause/resume, stop cancelling an in-flight turn, objective completion, terminal/logging errors, and queued human input |
| `tests/metrics_writer_tests.cpp` | header/tool/turn/footer reconciliation, the turn `calls` tally as an object or `null`, the footer's `calls`, `reward`, `reward_version`, `reward_weights`, and `rollout_id`, the incomplete footer on destruction, footer finality, that the header's Config fields are exactly the worker record's `config`, and that a log survives text that is not UTF-8 |
| `tests/session_tests.cpp` | config rejection, request-header rejection (Scry-managed names, malformed names and values, and the reserved `X-Pigpen-Rollout`) before any log is opened, that a session owns a seeded world plus a registered tool harness atomically, starting with no retained turns, the header's `rollout_id`, that its summary, duration, and footer use the reward weights it was created with, and that a session without a log directory writes nothing yet still summarises itself |
| `tests/episode_driver_tests.cpp` | `EpisodeDriver` against a fake episode with hand-driven time: finishing on its own, the deadline measured from the first step, a timeout whose cancellation finishes within the 15 s grace, a stalled cancellation, a stop request before the first pump waiting without limit, a stop request during the grace period keeping the grace deadline (stalling, or `interrupted` when the episode finishes in time), and idleness |
| `tests/episode_batch_tests.cpp` | `EpisodeBatch` against a fake episode factory with hand-driven time: start order and the parallel cap, reports in completion order exactly once while the episode is alive (and destruction right after), a per-episode deadline from each episode's first step, a stalled cancellation reported before the batch moves on, a stop request cancelling every live episode once and starting nothing more (never-started jobs are only counted), a stop winning over a timeout, factory failures aborting the batch the same way, a throwing factory or report aborting it without a double report, and `run()` sleeping 1 ms only after a pass without progress; with a job source, jobs pulled only while a slot is free, idling while the source is pending, ending once it is exhausted and live episodes finish, a stop while waiting for input, and a throwing source aborting the batch |
| `tests/worker_jobs_tests.cpp` | the worker's `--seeds` lists and ranges with each diagnostic and the job cap, rollout prefixes, seed-major jobs built on demand from their index (rollout ids and sampling seeds, including the 32-bit ceiling and a full-size batch), duplicate seeds, `--header` parsing with the worker's reserved names, `OptionParser::rejected()`, `--jobs -` job lines (every field, `null` sampling seeds, and each rejection with its message, including out-of-range numbers such as `1e400` in any key, repeated keys, and NUL bytes), and `JobStream`'s defaults, rollout-id checks, and uniqueness naming the earlier line |
| `tests/line_reader_tests.cpp` | `LineReader` over scripted byte sources: lines split across reads of any size, `\r` kept, blank lines numbered, a last line without a terminator, over-long lines flagged with their bytes dropped, the queue capacity holding the source back, a read error or a throwing source ending the input with its message (and the end of input with none), and destruction while a read is blocked |
| `tests/cli_options_tests.cpp` | the shared `OptionParser` (both value syntaxes for flag, text, integer, real, and free-form options, and each diagnostic's exact text), UTF-8 validation (truncated, overlong, surrogate, and out-of-range sequences), the shared Config flags (with or without `--seed`) and their validation order including the UTF-8 checks, the generated help (including every reward weight name), and `TerminationSignal` |
| `tests/world_animation_tests.cpp` | the typed activity feed becoming an ordered visual timeline, with caller-supplied time |
| `tests/gui_options_tests.cpp` | GUI startup parsing for model, endpoint, and reward arguments, including both value syntaxes, the GUI's refusal of an empty or non-UTF-8 model or endpoint, and its help text |

**CLI tests** registered in `cmake/testing.cmake`:

- `pigpen_headless_integration` — `tests/headless_integration_tests.py` runs
  the CLI against a loopback OpenAI-compatible stub through the real Scry/Curl
  path: argv wiring (including `--sampling-seed` and repeated `--reward`
  overrides), the tool result posted back to the provider, stdout (including
  the summary line's `reward=`), the JSONL log on disk (including each turn's
  call tally and the footer's reward breakdown), and exit codes `0` (a valid
  `move`) and `5` (only a schema-invalid call)
- `pigpen_headless_help` — `--help` exits 0
- `pigpen_headless_requires_model`, `pigpen_headless_rejects_invalid_bounds`
  (`--max-tool-rounds 65`), `pigpen_headless_rejects_invalid_temperature`
  (`--temperature nan`), `pigpen_headless_rejects_invalid_sampling_seed`
  (above the 32-bit range), `pigpen_headless_rejects_unknown_reward_weight`
  (`--reward bogus=1`), and `pigpen_headless_rejects_invalid_reward_value`
  (`--reward=invalid_call=inf`) — each passes only if the CLI prints the
  matching option diagnostic
- `pigpen_worker_integration` — `tests/worker_integration_tests.py` runs
  `pig-pen-worker` against a *threaded* loopback stub (a single-threaded one
  would serialise the parallel sessions and hide bugs) that scripts each
  rollout by its `X-Pigpen-Rollout` header. Three seeds × two samples with
  `--parallel 3`: every request carries the rollout, `X-Pigpen-Seed`, and a
  `--header` value, each rollout's two requests share one id, the stub holds
  each first request until three rollouts are in flight (an event with a
  timeout, so a serial worker fails rather than flakes) and asserts the peak
  was exactly three, stdout is exactly
  six `episode` records and one `batch` record, both samples of a seed get
  identical tool results and outcomes, each reward's terms and total add up,
  sampling seeds follow `--sampling-seed-base`, `--log-dir` writes six logs
  whose footers match the records, and the exit code is 0. Then an HTTP 400
  for one rollout and a never-answered request for another (`error` and
  `timeout` records, exit 6, no logs by default), usage errors (exit 2,
  nothing on stdout), and on UNIX: a consumer that closes stdout after the
  first record while two episodes are in flight (exit 1, an `output error`
  diagnostic, no further job started, and every started episode's log ending
  in a complete footer), and `SIGINT` and `SIGTERM` while two rollouts are in
  flight (two `stopped` records, an `interrupted` batch record with two jobs
  not started, exit 130 / 143). With `--jobs -`: job lines on stdin with
  defaults, an explicit rollout id, per-job and `null` sampling seeds (which
  fall back to `--sampling-seed`), a blank line, and seven rejected lines
  (invalid JSON, a repeated rollout id, an unknown key, an over-long line,
  `1e400`, a repeated key, a NUL byte),
  asserting the `job_error` records with their line numbers, the headers and
  request bodies, the batch record's `job_errors`, and exit 6; a producer that
  writes one job, waits for its record while stdin stays open, then writes the
  next (the worker is long-lived and does not wait for end of input); and, on
  UNIX, `SIGINT` while the worker waits on a stdin pipe whose write end the
  test holds open until the worker has exited (exit 130, an `interrupted`
  batch record; a worker that joined its blocked reader thread would hang
  and fail it), and a stdin that cannot be read (a directory: exit 1, an
  `aborted` batch record naming the input error)
- `pigpen_worker_help`, and `pigpen_worker_requires_model`, `_requires_seeds`,
  `_rejects_world_seed_flag`, `_rejects_malformed_seeds`,
  `_rejects_descending_seed_range`, `_rejects_duplicate_seed`,
  `_rejects_zero_samples`, `_rejects_zero_parallel`,
  `_rejects_sampling_seed_overflow`, `_rejects_two_sampling_seeds`,
  `_rejects_bad_rollout_prefix`, `_rejects_reserved_header`,
  `_rejects_unknown_reward_weight`, `_rejects_jobs_with_seeds`,
  `_rejects_jobs_with_samples`, and `_rejects_jobs_file` — each passes only
  if the worker prints the matching option diagnostic
- `pigpen_rollout_consumer` — `tests/rollout_consumer_tests.py` runs
  `examples/rollout_consumer.py` on canned worker records and a canned
  request log (every drop reason, trajectory order, plain and normalised
  group advantages, groups by rollout-id prefix and seed with the seed-only
  fallback, the stderr summary), through a fake worker command
  (including a failing one), and on a real `pig-pen-worker` batch joined
  against the requests the threaded loopback stub logged
- `pigpen_headless_rejects_invalid_utf8` / `pigpen_worker_rejects_invalid_utf8`
  (UNIX) — `tests/invalid_utf8_tests.py` passes raw non-UTF-8 bytes for
  `--model`, `--base-url`, `--prompt-variant` (and the headless `--input`)
  and asserts exit 2, the `must be valid UTF-8` diagnostic, nothing on
  stdout, and no log written
- `pigpen_headless_graceful_sigint` / `_sigterm` — `tests/headless_signal_tests.py`
  starts a stub socket server on a loopback port, points the CLI at it, sends
  an exact tagged model identifier, verifies that identifier in the HTTP
  request and JSONL header, then asserts the exit status is `128 + signal`
  *and* that the JSONL file still ends with a finalized footer whose reward
  is invalid (`stopped`), matching `reward=invalid` on the summary line

Python 3 is required whenever tests are enabled. The two headless signal
tests only register on UNIX, and the worker integration test skips its signal
scenarios elsewhere; the loopback integration tests run on every supported
platform.

`tests/bench/` holds a benchmark that is not part of the suite: built only
with `-DPIGPEN_BUILD_BENCH=ON` and run by hand, see
[Training](training.md#performance-notes).

## Running a subset

```sh
ctest --preset dev -R world              # by test name
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

`SCRY_BUILD_TESTING_SUPPORT` follows `PIGPEN_BUILD_TESTS`; the scripted component
is linked only into the reflection test binary and is absent from production-only
builds. Loopback tests remain to cover CLI startup, Curl, and OS signals.
