# Architecture

Pig Pen is a C++26 application in three layers plus two thin entry points. The
point of the layering is that everything except the final transport integration
is testable without a window or a model server.

```
src/app/main.cpp              src/app/headless_main.cpp
        │  SDL3 + ImGui loop          │  argv parsing, signals, stdout
        ▼                             ▼
   src/ui/  ─────────────►  agent::Session  ◄─────────────
   AppUi, WorldAnimation          │
                                  ├── EpisodeRunner   turn loop, play/pause/stop
                                  ├── ScryTurnTransport ──► scry::Harness ──► HTTP
                                  ├── WorldTools      typed actions and visibility
                                  ├── WorldToolBinding reflected scry::ToolRegistry
                                  ├── ToolActivity    typed semantics + exact payloads
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
| `prompt.cpp` | builds the system prompt from `Config` and per-turn instructions from feedback and optional human guidance |
| `tool_contract.hpp` | reflected argument and flat response declarations, including world outcome fields; these C++ types are the model-facing contract |
| `world_tools.cpp` | typed world actions and scenario visibility; it contains no JSON parsing, budgets, or schema code |
| `world_tool_binding.cpp` | standalone reflected registry, world-action admission policy (counting its own refusals per turn), and correlation of typed transitions with Scry dispatch observations |
| `events.hpp` | `ToolActivity` and its append-only feed: typed application semantics plus exact canonical argument/result text from Scry |
| `turn_transport.hpp` | `ITurnTransport`, the interface a "send one turn, get callbacks" implementation must satisfy |
| `scry_transport.cpp` | the real implementation, over `scry::Harness` / `scry::Conversation` |
| `episode_runner.cpp` | the state machine: `idle → playing ⇄ paused → finished`, turn budget, cooperative cancellation, transcript, and observers for turn/episode completion |
| `episode_turn.cpp` | `EpisodeTurn` (a `TurnRecord` plus its optional `TurnCallTally`) and the pure `tally_turn_calls()` that splits Scry's call count into executed, invalid, budget-refused, and host-refused |
| `metrics_writer.cpp` | JSONL header/tool/turn/footer, with a footer guaranteed even on abnormal shutdown |
| `session.cpp` | composes all of the above into one owned object |

Two seams make this testable. `ITurnTransport` lets `EpisodeRunner` be driven
by a scripted transport in `tests/episode_runner_tests.cpp`, so the whole turn
loop — including stop-cancels-in-flight-turn — is covered without a model.
`WorldTools` accepts and returns only reflected C++ values, so world behavior,
fixed response shapes, and scenario visibility are tested without JSON or a registry.
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
registered tools, the runner, and the metrics writer. There is no partial
reset: to start over, you destroy the session and create a new one, which is
exactly what the GUI's **Reset** button does. That is why connection and
scenario edits show "Pending settings apply on Reset" instead of mutating a
live episode.

The session is also where an episode's facts live: the world, the activity
feed, every finished turn with its call tally (`turns()`), and its creation
time (`elapsed()`). The log records the same facts but is not their only home.

The session owns the bindings and world at stable addresses. They outlive the
harness that adopts their registry. Destruction flushes pending world activity
while the runner and log are alive, then cancels and disconnects the transport.
Clients inspect the runner through a const view and control it through `Session`.
Scry validates provider configuration before a log is opened; Pig Pen adds only
its episode-budget and tool-round upper bounds.

### Everything is pumped, nothing blocks

`Session::pump()` gives the scry harness a 2 ms time budget and at most 32
callbacks, then ticks the runner. Both front ends call it from their own loop —
the GUI once per frame, the CLI in a tight loop that sleeps 1 ms when there is
nothing to do. Scry owns its I/O worker; application callbacks and tools run
only on the pump thread, with no blocking waits in either front end.

Cancellation is cooperative for the same reason: `stop()` asks the transport to
cancel and the episode is not finished until the terminal callback comes back,
which is what lets the footer be written before exit.

## `src/ui` — the ImGui layer

`AppUi` owns the `shared_ptr<Session>`, the control widgets, and the panel
drawing. `WorldAnimationState` is the interesting piece: a turn can produce a
burst of tool calls at once, so it converts the activity feed into a queue of
timed steps and plays them back one at a time, taking the current time as a
parameter. That keeps it free of any ImGui or wall-clock dependency, which is
why `tests/world_animation_tests.cpp` can test animation without a window —
`pigpen_ui` compiles it once and is shared by the GUI and the test binary.

## `src/app` — the entry points

`main.cpp` is SDL3/OpenGL/ImGui setup and the frame loop, nothing else.
`headless_main.cpp` is argument parsing, `SIGINT`/`SIGTERM` handling (the
handler only writes a `volatile sig_atomic_t`), incremental printing of the
transcript and activity feed, and the exit-code policy described in
[Running](running.md#exit-codes).

## Build layout

`CMakeLists.txt` describes Pig Pen's own targets; `cmake/dependencies.cmake`
pins and fetches the runtime dependencies and `cmake/testing.cmake` adds Catch2
and the suite. The static libraries are `pigpen_world`, `pigpen_agent`, and,
when the GUI or tests are enabled, `pigpen_ui`. `pigpen_agent` links
`scry::scry` privately: scry carries `-freflection`, so every `pigpen_agent`
translation unit (and `pigpen_reflection_tests`, which links scry directly)
compiles with it, while `pigpen_world`, `pigpen_ui`, and both entry points stay
reflection-free. nlohmann/json is likewise private to `pigpen_agent`'s metrics
writer. The `pigpen_target()` helper applies C++26 and the warning flags to
pig-pen's own targets only; fetched dependencies are `SYSTEM`. See
[Building](building.md).
