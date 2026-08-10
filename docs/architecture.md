# Architecture

Pig Pen is a C++26 application in three layers plus two thin entry points. The
point of the layering is that everything except the final transport integration
is testable without a window or a model server.

```
src/app/main.cpp              src/app/headless_main.cpp
        │  GLFW + ImGui loop          │  argv parsing, signals, stdout
        ▼                             ▼
   src/ui/  ─────────────►  agent::Session  ◄─────────────
   AppUi, WorldAnimation          │
                                  ├── EpisodeRunner        turn loop, play/pause/stop
                                  ├── ScryTurnTransport ──► scry::Harness ──► HTTP
                                  ├── WorldToolController  per-call orchestration
                                  ├── WorldTools           typed actions and budgets
                                  ├── ToolActivityJournal  typed activity feed + counters
                                  ├── Scry reflection schemas and marshalling
                                  ├── MetricsWriter        JSONL serialization boundary
                                  └── world::World         the simulation
```

JSON exists only at the two external boundaries: scry owns the model-facing
schema/decode/encode, and `MetricsWriter` owns the persistent JSONL log.
Everything between them — the tool handlers, the activity feed, the UI, the
animation, the CLI output — passes typed C++ values:

```text
model <-> scry JSON/reflection <-> typed handlers (WorldToolController)
                                        │
                                        ▼
                              ToolActivity (typed)
                               │             │
                               ▼             ▼
                              UI        MetricsWriter (JSONL)
```

## `src/world` — the simulation

`World` is a value type: seed, blob position, score, a flat item array, an
observed bitset, per-item eaten counts. No JSON, no networking, no callbacks.
Actions return typed results (`MoveResult`, `LookResult`, `EatResult`) with
enumerated failures rather than strings, and `dump()` gives a canonical
serialisation used for determinism tests. Details in [World and tools](world.md).

## `src/agent` — the runtime

| unit | responsibility |
|---|---|
| `config.hpp` | `Config`: endpoint, model, seed, budgets, and the three model-visibility flags shared by both front ends |
| `prompt.cpp` | builds the system prompt and the per-turn nudge from a `Config` |
| `tool_contract.hpp` | reflected argument and flat response declarations, including status and budget fields; these C++ types are the model-facing contract |
| `world_tools.cpp` | typed world actions and the explicit per-turn action-budget lifecycle; it contains no JSON parsing or schema code |
| `world_tool_controller.cpp` | the application service decoded calls land on: opens the turn's budget window, invokes `WorldTools`, publishes exactly one `ToolActivity`, and returns only the typed response to scry |
| `tool_activity.hpp` | `ToolKind`, `ToolOutcome`, and the typed `ToolActivity` record, plus the `IToolActivitySink` seam the controller publishes through |
| `tool_activity_journal.cpp` | the append-only activity history: tick stamping, per-tool and per-item counters, and forwarding to `MetricsWriter` with a latched failure the session consumes |
| `turn_transport.hpp` | `ITurnTransport`, the interface a "send one turn, get callbacks" implementation must satisfy |
| `scry_transport.cpp` | the real implementation, over `scry::Harness` / `scry::Conversation` |
| `episode_runner.cpp` | the state machine: `idle → playing ⇄ paused → finished`, turn budget, cooperative cancellation, transcript, and observers for turn/episode completion |
| `metrics_writer.cpp` | JSONL header/tool/turn/footer, with a footer guaranteed even on abnormal shutdown |
| `session.cpp` | composes all of the above into one owned object |

Three seams make this testable. `ITurnTransport` lets `EpisodeRunner` be
driven by a scripted transport in `tests/episode_runner_tests.cpp`, so the
whole turn loop — including stop-cancels-in-flight-turn — is covered without a
model. `WorldTools` accepts and returns only reflected C++ values, so world
behavior, fixed response shapes, and budgets are tested without JSON or a
registry. `IToolActivitySink` lets `WorldToolController` be tested against a
recording sink, so per-call orchestration and summaries are covered without a
session, a journal, or a log file.

### Reflection is the tool boundary

`Session` registers `DirectionArguments` and `EatArguments` through
`scry::reflection::add` as metadata plus thin typed callables that forward to
`WorldToolController`. Scry derives closed JSON Schemas at compile time,
strictly decodes incoming arguments, invokes the typed handler on the pump
thread, and encodes its typed response. Scoped enum identifiers are the JSON
strings, so adding or renaming a direction changes schema, decode, and encode
from the same declaration.

Protocol failures belong to Scry: unknown tools and calls that cannot be
decoded never enter `WorldToolController`, the world, the journal, or the
four-action application budget. Pig Pen's activity journal therefore
represents successfully decoded world-tool handler invocations. A decoded
call rejected by the budget still publishes one activity, with
`ToolOutcome::budget_exhausted` and no world mutation.

### Tool activity is typed end to end

Every decoded call produces exactly one `ToolActivity` — tool kind, outcome,
turn/tick, before/after positions, optional direction and eaten item, the
truthful score, and a presentation summary built once at publication. The
journal appends it, maintains call/eaten counters, and forwards the record to
`MetricsWriter`; the UI panels, animation, headless printing, and statistics
all reconcile from that same record through read-only accessors. None of them
parse JSON, and the three scenario visibility flags never distort the record:
the activity and the log always describe the truth, even when the model is
told less.

The committed typed response always reaches scry, even if persistence fails:
the journal latches the first metrics failure and `Session::pump()` fails the
episode only after `Harness::update` has returned the response.

### `Session` is the reset unit

A `Session` owns the world, the conversation, the scry harness with its
registered tools, the controller, the journal, the runner, and the metrics
writer. There is no partial
reset: to start over, you destroy the session and create a new one, which is
exactly what the GUI's **Reset** button does. That is why connection and
scenario edits show "Pending settings apply on Reset" instead of mutating a
live episode.

Tools are registered on the harness with a `weak_ptr` back to the session, so a
callback arriving after the session is gone fails cleanly instead of touching
freed state.

### Everything is pumped, nothing blocks

`Session::pump()` gives the scry harness a 2 ms time budget and at most 32
callbacks, then ticks the runner. Both front ends call it from their own loop —
the GUI once per frame, the CLI in a tight loop that sleeps 1 ms when there is
nothing to do. No background threads, no blocking waits, and the same code path
in both.

Cancellation is cooperative for the same reason: `stop()` asks the transport to
cancel and the episode is not finished until the terminal callback comes back,
which is what lets the footer be written before exit.

## `src/ui` — the ImGui layer

`AppUi` owns the `shared_ptr<Session>`, the control widgets, and the panel
drawing; every panel reads typed `ToolActivity` fields from the journal.
`WorldAnimationState` is the interesting piece: a turn can produce a burst of
tool calls at once, so it converts the activity history into a queue of timed
steps and plays them back one at a time, taking the current time as a
parameter. That keeps it free of any ImGui or wall-clock dependency, which is
why `tests/world_animation_tests.cpp` can test animation without a window —
and it is also compiled into the test binary directly for that reason.

## `src/app` — the entry points

`main.cpp` is GLFW/OpenGL/ImGui setup and the frame loop, nothing else.
`headless_main.cpp` is argument parsing, `SIGINT`/`SIGTERM` handling (the
handler only writes a `volatile sig_atomic_t`), incremental printing of the
transcript and activity journal, and the exit-code policy described in
[Running](running.md#exit-codes).

## Build layout

`CMakeLists.txt` requires GCC 16+, C++26, and Scry's reflection capability
probe. It builds `pigpen_world`, `pigpen_reflected_tools`, and `pigpen_agent`
as focused static libraries, then the GUI, headless program, and test executable
from explicit source lists. Reflection compiler requirements stay scoped to the
agent/tool boundary instead of leaking into fetched dependencies or front-end
translation units, and nlohmann is a PRIVATE dependency of `pigpen_agent`, so
UI and app translation units cannot even include JSON headers.
Warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`) apply
through the `pigpen_project_options` interface target to pig-pen's own code
only; fetched dependencies are added as `SYSTEM` with their tests and examples
turned off.
See [Building](building.md).
