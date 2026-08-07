# Architecture

About 3,900 lines of C++23 under `src/` in three layers, plus two thin `main`s
and roughly 1,200 lines of tests. The point of the layering is that everything
except the last layer is testable without a window or a model server.

```
src/app/main.cpp              src/app/headless_main.cpp
        │  GLFW + ImGui loop          │  argv parsing, signals, stdout
        ▼                             ▼
   src/ui/  ─────────────►  agent::Session  ◄─────────────
   AppUi, WorldAnimation          │
                                  ├── EpisodeRunner   turn loop, play/pause/stop
                                  ├── ScryTurnTransport ──► scry::Harness ──► HTTP
                                  ├── ToolExecutor    schemas, validation, events
                                  ├── MetricsWriter   JSONL
                                  └── world::World    the simulation
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
| `tool_executor.cpp` | the model-facing boundary: publishes the three JSON schemas, validates arguments exactly, applies the action, shapes the visible result, and appends one `WorldEvent` per invocation — including rejected ones |
| `events.hpp` | `WorldEvent` and the append-only `EventFeed` that the UI, animation, and logger all read |
| `turn_transport.hpp` | `ITurnTransport`, the interface a "send one turn, get callbacks" implementation must satisfy |
| `scry_transport.cpp` | the real implementation, over `scry::Harness` / `scry::Conversation` |
| `episode_runner.cpp` | the state machine: `idle → playing ⇄ paused → finished`, turn budget, cooperative cancellation, transcript, and observers for turn/episode completion |
| `metrics_writer.cpp` | JSONL header/tool/turn/footer, with a footer guaranteed even on abnormal shutdown |
| `session.cpp` | composes all of the above into one owned object |

Two seams make this testable. `ITurnTransport` lets `EpisodeRunner` be driven
by a scripted transport in `tests/episode_runner_tests.cpp`, so the whole turn
loop — including stop-cancels-in-flight-turn — is covered without a model.
`ToolExecutor` holds no world state and speaks only JSON in and JSON out, so
the exact model-visible contract is asserted directly.

### `Session` is the reset unit

A `Session` owns the world, the conversation, the scry harness with its
registered tools, the runner, and the metrics writer. There is no partial
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
drawing. `WorldAnimationState` is the interesting piece: a turn can produce a
burst of tool calls at once, so it converts the event feed into a queue of
timed steps and plays them back one at a time, taking the current time as a
parameter. That keeps it free of any ImGui or wall-clock dependency, which is
why `tests/world_animation_tests.cpp` can test animation without a window —
and it is also compiled into the test binary directly for that reason.

## `src/app` — the entry points

`main.cpp` is GLFW/OpenGL/ImGui setup and the frame loop, nothing else.
`headless_main.cpp` is argument parsing, `SIGINT`/`SIGTERM` handling (the
handler only writes a `volatile sig_atomic_t`), incremental printing of the
transcript and event feed, and the exit-code policy described in
[Running](running.md#exit-codes).

## Build layout

`CMakeLists.txt` builds `pigpen_world` and `pigpen_agent` as static libraries,
then three executables on top. Source files are globbed with
`CONFIGURE_DEPENDS`, and the target definitions guard on file existence so the
project stays configurable while a module is being added. Warnings
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`) apply through the
`pigpen_project_options` interface target to pig-pen's own code only; fetched
dependencies are added as `SYSTEM` with their tests and examples turned off.
See [Building](building.md).
