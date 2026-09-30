# Architecture

Pig Pen is a C++26 application in four layers plus three thin entry points.
The point of the layering is that everything except the final transport integration
is testable without a window or a model server.

```
src/app/main.cpp          src/app/headless_main.cpp       src/app/worker_main.cpp
  SDL3 + ImGui frame loop   eval CLI: one episode,          RL worker: seeds × samples
                            transcript, exit codes          or stdin jobs, P in flight,
                                                            JSONL records
        │                         │                               │
   src/ui/  AppUi,           src/cli/  OptionParser, shared Config flags,
   WorldAnimation            TerminationSignal, worker jobs, LineReader
   (GUI options via src/cli)      │                               │
        │                         │                     agent::EpisodeBatch
        │                         │                       job queue, P drivers
        │                  agent::EpisodeDriver ◄─────────────────┘
        │                    deadline, cancellation grace
        ▼                         ▼
   agent::Session  (Config: the episode, SessionOptions: the host)
                                  │
                                  ├── EpisodeRunner   turn loop, play/pause/stop
                                  ├── ScryTurnTransport ──► scry::Harness ──► HTTP
                                  ├── WorldTools      typed actions and visibility
                                  ├── WorldToolBinding reflected scry::ToolRegistry
                                  ├── ToolActivity    typed semantics + exact payloads
                                  ├── summarize_episode() → EpisodeSummary + reward
                                  ├── MetricsWriter   JSONL, only with a log directory
                                  └── world::World    the simulation
```

The episode never knows which front end drives it: all three run the same
`Session`, and a front end differs only in who owns the pump loop, how many
sessions it pumps, and what it does with the facts an episode leaves behind.

## `src/world` — the simulation

`World` is a value type: seed, blob position, score, a flat item array, an
observed bitset, per-item eaten counts. No JSON, no networking, no callbacks.
Actions return typed results (`MoveResult`, `LookResult`, `EatResult`) with
enumerated failures rather than strings, and `dump()` gives a canonical
serialisation used for determinism tests. Details in [World and tools](world.md).

## `src/agent` — the runtime

| unit | responsibility |
|---|---|
| `config.hpp` | `Config`, the episode contract: endpoint, model, seed, budgets, sampling, and the three model-visibility flags; everything that changes what the model sees or that the log header records |
| `session_options.hpp` | `SessionOptions`, host plumbing: the optional log directory, `prompt_variant` label, API key, rollout id, extra request headers, and reward weights. A front end's mode is expressed only through these and through which loop it owns; there is no mode enum in `agent/` |
| `prompt.cpp` | builds the system prompt from `Config` and per-turn instructions from feedback and optional human guidance |
| `tool_contract.hpp` | reflected argument and flat response declarations, including world outcome fields; these C++ types are the model-facing contract |
| `world_tools.cpp` | typed world actions and scenario visibility; it contains no JSON parsing, budgets, or schema code |
| `world_tool_binding.cpp` | standalone reflected registry, world-action admission policy (counting its own refusals per turn), and correlation of typed transitions with Scry dispatch observations |
| `events.hpp` | `ToolActivity` and its append-only feed: typed application semantics plus exact canonical argument/result text from Scry |
| `turn_transport.hpp` | `ITurnTransport`, the interface a "send one turn, get callbacks" implementation must satisfy |
| `scry_transport.cpp` | the real implementation, over `scry::Harness` / `scry::Conversation`; `scry_config()` maps a `Config` and `SessionOptions` (credential, request headers, `X-Pigpen-Rollout`) to scry's provider configuration |
| `episode_runner.cpp` | the state machine: `idle → playing ⇄ paused → finished`, turn budget, cooperative cancellation, transcript, and observers for turn/episode completion |
| `episode_turn.cpp` | `EpisodeTurn` (a `TurnRecord` plus its optional `TurnCallTally`) and the pure `tally_turn_calls()` that splits Scry's call count into executed, invalid, budget-refused, and host-refused |
| `reward.cpp` | `RewardWeights`, `EpisodeFacts`, and the pure `compute_reward()` returning a `RewardBreakdown` (validity, raw counts, per-term contributions); `parse_reward_weight()` for `NAME=VALUE` overrides. No scry, JSON, or `Session` |
| `episode_summary.cpp` | `episode_facts()` and `summarize_episode()`: an episode's outcome (finish reason, turns, score, items eaten, tool counts, summed call tally, duration, reward) from a world, activity feed, turns, and runner snapshot, or from a `Session` |
| `summary_json.cpp` | `to_json_line()`, the only serialisation of a summary, its reward, and its weights, and of the worker's `EpisodeRecord` (the summary plus seed, sample, and `Config`), `BatchRecord`, and `JobErrorRecord`. `record_json.hpp`, internal to `pigpen_agent`, shares `config_json()` so the log header and the worker record write `Config` one way |
| `job_spec.cpp` | `parse_job_spec()`: one `--jobs -` line into a plain `JobSpec` (seed, optional sample, rollout id, and sampling seed), rejecting anything else with a message; it lives here only because nlohmann/json does. Defaults and uniqueness are the worker's |
| `metrics_writer.cpp` | JSONL header/tool/turn/footer; a finished episode's footer is its serialised summary, and a footer is guaranteed even on abnormal shutdown |
| `episode_driver.cpp` | `IDrivableEpisode` (`pump`, `finished`, `stop`) and `EpisodeDriver`, the one copy of the drive-to-completion policy: overall deadline, cooperative cancellation, the 15 s grace, and a stop request that waits without limit unless a timeout's grace period is already running. Time is a parameter, so it is tested with a fake episode |
| `episode_batch.cpp` | `EpisodeBatch`: jobs by index, either a known count or a job source asked only while a slot is free (`ready`, `pending`, or `exhausted`, so a stream is pulled lazily), a `std::function` factory that turns a job into a `BatchEntry` (an owned `IDrivableEpisode` plus its `on_end` report), and up to P `EpisodeDriver`s pumped round-robin on one thread. Reports come exactly once, in completion order, while the episode is alive; a stop request or a factory failure stops starting jobs and cancels every live one. `run()` takes the clock and the idle sleep, so it is tested with fakes |
| `session.cpp` | composes all of the above into one owned object, and implements `IDrivableEpisode` |

Three seams make this testable. `ITurnTransport` lets `EpisodeRunner` be driven
by a scripted transport in `tests/episode_runner_tests.cpp`, so the whole turn
loop — including stop-cancels-in-flight-turn — is covered without a model.
`WorldTools` accepts and returns only reflected C++ values, so world behavior,
fixed response shapes, and scenario visibility are tested without JSON or a registry.
`IDrivableEpisode` lets `EpisodeDriver`'s deadline and cancellation policy,
and `EpisodeBatch`'s scheduling on top of it, be tested with fake episodes
and hand-driven time.
The public `scry::testing` component exercises the real bindings, transport, tool
budgets, and transactional history with scripted provider streams.

### Reflection is the tool boundary

`WorldToolBinding` builds a standalone `scry::ToolRegistry` with reflected
`DirectionArguments` and `EatArguments` handlers before the harness is created.
Scry derives closed JSON Schemas at compile time, strictly decodes arguments,
invokes the typed handler on the pump thread, and encodes its response. Scoped
enum identifiers supply the JSON strings from the same C++ declaration.

Contextual handlers retain the typed world transition and Scry's turn/call ID,
round, and batch index. The subsequent `on_tool_call` observation supplies the
exact canonical arguments and result posted for the provider. One pending
transition suffices because dispatch and observation are serial. There is no
second encode for logging. If Scry aborts the whole turn before posting a result,
it emits no observation. The pending world transition is still recorded at turn
completion (or session destruction), with `result_dispatched: false` and null
JSON payloads. World side effects remain visible even when history rolls back.

Scry enforces the four-request limit before dispatch. Invalid/unknown requests
spend the budget too; refused and undecodable requests never become world
activity. The admission hook prevents further world changes after objective
completion or logging failure. Logging failure ends the episode after the active
turn terminates, allowing executed results to commit when the turn succeeds.
The binding counts its own refusals; `complete_turn()` flushes pending activity
and returns and resets that count on every terminal delivery. Scry's rejected
count includes both kinds of refusal, so subtracting the host's count leaves the
budget refusals, and whatever is neither rejected nor executed was invalid.

Pig Pen uses Scry's completing round-limit policy, so hitting the round cap keeps
the executed transcript. `TurnOutcome` carries native completion counts and the
number of calls left unexecuted; `EpisodeRunner` passes these to metrics and
notifies the model about unexecuted calls in the next turn's prompt. World-action
counts remain separate because invalid and refused requests are included in
Scry's counts. When Scry fails/cancels without a Completion, native statistics
are absent.

### `Session` is the reset unit

A `Session` owns the world, the conversation, the scry harness with its
registered tools, the runner, and the metrics writer. It is created from a
`Config` (the episode) and `SessionOptions` (the host's choices). Without
`SessionOptions::log_directory` no `MetricsWriter` exists at all:
`metrics_path()` is empty, `metrics_error()` stays empty, and every fact
below is still recorded. The session never reads the environment; the front
ends pass `PIGPEN_API_KEY` in as `SessionOptions::api_key`. There is no partial
reset: to start over, you destroy the session and create a new one, which is
exactly what the GUI's **Reset** button does. That is why connection and
scenario edits show "Pending settings apply on Reset" instead of mutating a
live episode.

The session is also where an episode's facts live: the world, the activity
feed, every finished turn with its call tally (`turns()`), and its duration
(`elapsed()`, which stops when the episode finishes), and its `rollout_id()`. The log records the same
facts but is not their only home. `summarize_episode()` builds an
`EpisodeSummary` from them at any time; the session writes that summary as
the footer when the episode finishes, using the `RewardWeights` it was created
with, and front ends call it for their own reports (the CLI's `reward=` and
the GUI's Stats panel). The shaped reward is computed only from these facts
and never feeds back into the world or the prompt.

The session owns the bindings and world at stable addresses. They outlive the
harness that adopts their registry. Destruction flushes pending world activity
while the runner and log are alive, then cancels and disconnects the transport.
Clients inspect the runner through a const view and control it through `Session`.
Scry validates provider configuration, including request headers that collide
with its own, before a log is opened; Pig Pen adds only its episode-budget and
tool-round upper bounds and reserves `X-Pigpen-Rollout`, which it sets itself
from `SessionOptions::rollout_id` (and records as the log header's
`rollout_id`).

### Everything is pumped, nothing blocks

`Session::pump()` gives the scry harness a 2 ms time budget and at most 32
callbacks, then ticks the runner. Every front end calls it from its own loop —
the GUI once per frame, the CLI through `EpisodeDriver::step()` in a loop that
sleeps 1 ms when the last pump had nothing to do, and the worker through
`EpisodeBatch`, which steps up to P drivers per pass and sleeps 1 ms only when
none of them had anything to do. Scry owns its I/O worker (one per session);
application callbacks and tools run only on the pump thread, with no blocking
waits in any front end.

Cancellation is cooperative for the same reason: `stop()` asks the transport to
cancel and the episode is not finished until the terminal callback comes back,
which is what lets the footer be written before exit. `EpisodeDriver` owns
that policy for a loop that must end: each `step(now, stop_requested)` honours
a new stop request, pumps, reports a finished episode, then checks the
deadline (measured from the first step). A passed deadline requests `stop()`
once and allows 15 s more; if cancellation outlasts that, the drive ends
`cancellation_stalled` with the episode unfinished. A stop request (a signal)
calls `stop()`; made before any timeout, it then waits without limit, and
made during the grace period it leaves the grace deadline in place, so an
episode stuck in cancellation cannot hold the process forever. The outcomes are `finished`,
`timed_out`, `cancellation_stalled`, and `interrupted`; the driver never
sleeps or reads a clock, so one thread can drive several episodes.

`EpisodeBatch` is that one thread's loop for the worker: each pass starts
jobs while fewer than P episodes are live and its job source has one ready,
then steps every live driver once. With `--jobs -` the source is stdin: a
`LineReader` thread does the only blocking read and queues lines, and the
source takes them without waiting, so a quiet stdin never stalls live
episodes; a pending source counts as no progress, so the batch idles as
usual, and an exhausted one ends the batch once the live episodes finish. A finished drive is reported, then its driver and session are
destroyed (driver first, since it borrows the episode), which finalises any
log and releases the harness before the next job starts. `run()` sleeps
1 ms only after a pass in which no job started, no drive ended, and every
pump was idle. Each session keeps its own scry I/O thread, so P sessions
need no locking, and every callback, tool handler, and record write still
happens on the one pump thread. A signal is a stop request for every live
driver; a job whose session cannot be created stops the batch the same way,
so every started episode is reported either way, and jobs never started
are only counted. An exception from the factory or from a report aborts the
batch the same way, and the slot is released regardless, so nothing is
reported twice. The worker also ignores `SIGPIPE` and treats a failed stdout
write as a stop request, so a closed pipe still cancels cooperatively.

The pump budget and per-job session creation were measured rather than
made configurable: with 128 sessions against an instant loopback server the
pump thread is at most about a quarter busy and a pass takes about 1 ms at
p99, and a session costs about 0.2 ms to create and destroy. The numbers are in
[Training](training.md#performance-notes).

## `src/ui` — the ImGui layer

`AppUi` owns the `shared_ptr<Session>`, the control widgets, and the panel
drawing. `WorldAnimationState` is the interesting piece: a turn can produce a
burst of tool calls at once, so it converts the activity feed into a queue of
timed steps and plays them back one at a time, taking the current time as a
parameter. That keeps it free of any ImGui or wall-clock dependency, which is
why `tests/world_animation_tests.cpp` can test animation without a window —
`pigpen_ui` compiles it once and is shared by the GUI and the test binary.

## `src/cli` — shared command-line plumbing

`pigpen_cli` holds what every command line needs and compiles against
neither scry nor a JSON library (job lines are parsed by `pigpen_agent`'s
`parse_job_spec()`). `OptionParser` is table-driven: `flag`, `text`, `integer`, `real`, and
free-form `value` registrations bind an option to the variable it fills, accept
`--option value` and `--option=value`, report every problem in one fixed
wording, and generate the help text. `add_config_options()` registers every
`Config` flag plus `--prompt-variant` and `--reward` (whose help lists
`reward_weight_fields`), and `validate_config_options()` rejects the values a
parse can leave empty. `TerminationSignal` installs the `SIGINT`/`SIGTERM`
handlers, which only write a `volatile sig_atomic_t` for the loop to poll.
`worker_jobs.cpp` is the worker's pure job arithmetic: `--seeds` lists and
ranges, seed-major expansion into `WorkerJob`s with rollout ids and sampling
seeds, `--header` parsing that keeps `X-Pigpen-Rollout` and
`X-Pigpen-Seed` for the worker, and `JobStream`, which turns `--jobs -`
lines into jobs (defaults, rollout-id checks, and uniqueness across the
stream). `LineReader` reads lines on its own thread and queues them for a
pump loop to poll, reporting a read error apart from the end of input; the
worker gives it standard input through the operating system's read call
rather than stdio, so a read still blocked when the process exits holds no
lock that exit needs. A front end that chooses world seeds itself
registers the shared flags without `--seed` and points `--seed` at its own
flag with `OptionParser::rejected()`.

## `src/app` — the entry points

`main.cpp` is SDL3/OpenGL/ImGui setup and the frame loop, nothing else.
`headless_main.cpp` is the usage text, its four own options (`--log-dir`,
`--timeout-seconds`, `--input`, `--help`), incremental printing of the
transcript and activity feed, a `while (!driver.step(...))` loop, and the
exit-code policy described in [Running](running.md#exit-codes).
`worker_main.cpp` is the usage text, the worker's own options, where jobs
come from (the command line's seeds × samples or stdin), the factory that
turns a job into a `Session` (world seed, sampling seed, rollout id, and
the `X-Pigpen-Seed` header), the records on stdout, and the exit-code policy
described in [Training](training.md#exit-codes). All three read
`PIGPEN_API_KEY` and pass it to the session.

## Build layout

`CMakeLists.txt` describes Pig Pen's own targets; `cmake/dependencies.cmake`
pins and fetches the runtime dependencies and `cmake/testing.cmake` adds Catch2
and the suite. The static libraries are `pigpen_world`, `pigpen_agent`,
`pigpen_cli`, and, when the GUI or tests are enabled, `pigpen_ui` (which uses
`pigpen_cli` for the GUI's options). `pigpen_agent` links
`scry::scry` privately: scry carries `-freflection`, so every `pigpen_agent`
translation unit (and `pigpen_reflection_tests`, which links scry directly)
compiles with it, while `pigpen_world`, `pigpen_cli`, `pigpen_ui`, and all
three entry points stay reflection-free: they include only scry-free agent headers,
and a private link dependency of a static library is link-only. nlohmann/json is likewise private to `pigpen_agent`, used
only by the metrics writer, `summary_json.cpp` (through the internal
`record_json.hpp`), and `job_spec.cpp`. The `pigpen_target()` helper applies C++26 and the warning flags to
pig-pen's own targets only; fetched dependencies are `SYSTEM`. See
[Building](building.md).
