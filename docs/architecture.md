# Architecture

Pig Pen is a C++23 application. It has four layers and two thin entry points.
Only `src/agent` uses C++26 reflection, and it is the only layer that
communicates with Scry. With these layers, you can test all code except the
final transport integration without a window or a model server.

```
src/app/main.cpp              src/app/headless_main.cpp
        │  SDL3 + ImGui loop          │  argv parsing, signals, stdout
        ▼                             ▼
   src/ui/  ─────────────►  agent::Session  ◄─────────────
   AppUi, WorldAnimation          │
                                  ├── core::EpisodeRunner   turn loop, play/pause/stop
                                  ├── ScryTurnTransport ──► scry::Harness ──► HTTP
                                  ├── core::WorldTools      typed actions and visibility
                                  ├── WorldToolBinding      Scry toolbox over WorldTools
                                  ├── core::ToolActivity    typed semantics + exact payloads
                                  ├── MetricsWriter         reflected JSONL
                                  └── world::World          the simulation
```

## `src/world`: the simulation

`World` is a value type. It contains the seed, the blob position, the score, a
flat item array, an observed bitset, and the eaten count for each item. It does
not use JSON, networks, or callbacks. Actions return typed results
(`MoveResult`, `LookResult`, `EatResult`) with enumerated failures, not
strings. `dump()` gives a canonical serialization for the determinism tests.
For more data, refer to [World and tools](world.md).

## `src/core`: the runtime without reflection

This layer is plain C++23 in the namespace `pigpen::core`. The build makes it
as `pigpen_core`. Code in this layer cannot include a Scry header.

| unit | responsibility |
|---|---|
| `config.hpp` | `Config`: the endpoint, model, seed, budgets, and the three model-visibility flags. Both front ends use it. |
| `prompt.cpp` | Creates the system prompt from `Config`. Creates the instructions for each turn from the feedback and the optional human guidance. |
| `tool_responses.hpp` | Flat response aggregates, with the world outcome fields. Scry reflects over them in `src/agent`. |
| `world_tools.cpp` | Typed world actions and scenario visibility. It takes world values. It contains no JSON, reflection, budgets, or schema code. |
| `events.hpp` | `ToolActivity` and its append-only feed. Each item has typed application semantics, and the exact canonical argument text and result text from Scry. |
| `turn_transport.hpp` | `ITurnTransport`, the interface for an implementation that sends one turn and gets callbacks. |
| `episode_runner.cpp` | The state machine (`idle → playing ⇄ paused → finished`), the turn budget, cooperative cancellation, the transcript, and observers for turn completion and episode completion. |

## `src/agent`: the Scry integration

This layer is C++26 with reflection, in the namespace `pigpen::agent`. The
build makes it as `pigpen_agent`. Put code here only when reflection replaces
hand-written shape code. Keep the public headers of this layer
C++23-parseable, so that the front ends can include them.

| unit | responsibility |
|---|---|
| `tool_contract.hpp` | The annotated `DirectionArguments`, and the checks that all argument types and response types are reflectable. This file and `core/tool_responses.hpp` together are the contract with the model. |
| `world_tool_binding.cpp` | The Scry toolbox (`move`, `look`, `eat`) over `core::WorldTools`, the admission policy for world actions, and the correlation of typed transitions with Scry dispatch observations. |
| `scry_transport.cpp` | The real `ITurnTransport`, over `scry::Harness` and `scry::Conversation`. |
| `metrics_writer.cpp` | The JSONL header, tool, turn, and footer records as reflected records. The writer always writes a footer, also after an abnormal shutdown. |
| `session.cpp` | Puts all of the above units into one owned object. |

Two seams make this layer testable:

- With `ITurnTransport`, a scripted transport in
  `tests/episode_runner_tests.cpp` can operate `EpisodeRunner`. The tests use
  this seam to cover the full turn loop without a model. This includes a stop
  that cancels an active turn.
- `WorldTools` takes world values and returns the plain response types. The
  tests use this seam to examine world behavior, fixed response shapes, and
  scenario visibility without reflection, JSON, or a registry.

The public `scry::testing::ScriptedServer` tests the real bindings, the
transport, the tool budgets, and the transactional history. It is a scripted
HTTP server on loopback, so these tests also run libcurl and the Scry stream
decoders.

### Reflection is the tool boundary

`WorldToolBinding` is a Scry toolbox. Its `move`, `look`, and `eat` member
functions have `scry::reflection::tool` annotations. Before Pig Pen creates the
harness, `Session` adds a `std::shared_ptr` to the binding to a standalone
`scry::ToolRegistry`. The registry shares the ownership of the binding.
Scry then does these steps for each tool:

- It gives the tool the name of its function.
- At compile time, it makes a closed JSON Schema from the parameters
  (`DirectionArguments`, or no parameters for `eat`).
- It strictly decodes the arguments.
- It calls the member function on the pump thread.
- It encodes the response.

The identifiers of the scoped enums supply the JSON strings from the same C++
declaration.

Each member function takes a `scry::ToolCallContext` as its first parameter.
It keeps the typed world transition, and the Scry turn ID, call ID, round, and
batch index. The subsequent `on_tool_call` observation supplies the exact
canonical arguments and result that Scry posted for the provider. Dispatch and
observation are serial, so one pending transition is sufficient. Pig Pen does
not encode the payloads a second time for the log.

If Scry aborts the full turn before it posts a result, it does not send an
observation. Pig Pen still records the pending world transition at turn
completion (or at session destruction). This record has
`result_dispatched: false` and `null` JSON payloads. World side effects stay
visible also when Scry rolls back the history.

Scry enforces the four-request limit before dispatch. Requests that are not
valid and requests for unknown tools also use the budget. Refused requests and
undecodable requests never become world activity. After objective completion
or a log failure, the admission hook stops all other world changes. A log
failure ends the episode after the active turn ends. If the turn is
successful, the results of the executed calls can commit.

Pig Pen uses the Scry round-limit policy that completes the turn. When a turn
gets to the round limit, Scry keeps the transcript of the executed calls.
`TurnOutcome` contains the native completion counts and the number of calls
that did not run. `EpisodeRunner` sends these values to the metrics writer. In
the prompt of the next turn, it also tells the model about the calls that did
not run.

Pig Pen keeps separate counts of world actions, because the Scry counts include
requests that are not valid and refused requests. If Scry fails or cancels
without a Completion, the native statistics are not available.

### `Session` is the reset unit

A `Session` owns the world, the conversation, the Scry harness with its
registered tools, the runner, and the metrics writer. A partial reset is not
possible. To start again, destroy the session and create a new one. The GUI
**Reset** button does exactly this. For this reason, changes to the connection
and the scenario do not change an active episode. The GUI shows "Pending
settings apply on Reset" instead.

The session keeps the world at a stable address. It exists longer than the
harness that adopts the registry. The session and the registry share the
bindings. During destruction, the
session first flushes pending world activity while the runner and the log are
still available. Then it cancels and disconnects the transport.

Clients examine the runner through a const view, and control it through
`Session`. Scry validates the provider configuration before Pig Pen opens a
log. Pig Pen adds only its upper limits for the episode budget and the tool
rounds.

### The front ends pump the session

`Session::pump()` gives the Scry harness a time budget of 2 ms and a maximum of
32 callbacks. Then it ticks the runner. Both front ends call `pump()` from
their own loop:

- The GUI calls it one time for each frame.
- The CLI calls it in a tight loop. When there is no work, the loop sleeps for
  1 ms.

Scry owns its I/O worker. Application callbacks and tools run only on the pump
thread. Neither front end blocks to wait for a result.

For the same reason, cancellation is cooperative. `stop()` asks the transport
to cancel. The episode does not finish until the terminal callback arrives.
This makes sure that Pig Pen writes the footer before it exits.

## `src/ui`: the ImGui layer

`AppUi` owns the `shared_ptr<Session>`, the control widgets, and the code that
draws the panels. `WorldAnimationState` controls the animation. One turn can
make a burst of tool calls at the same time. `WorldAnimationState` changes the
activity feed into a queue of timed steps, and shows these steps one at a
time. It takes the current time as a parameter, so it has no dependency on
ImGui or on the wall clock.

Because of this, `tests/world_animation_tests.cpp` can test the animation
without a window. `pigpen_ui` compiles `WorldAnimationState` one time. The GUI
and the test binary both link `pigpen_ui`.

## `src/app`: the entry points

`main.cpp` contains only the SDL3, OpenGL, and ImGui setup and the frame loop.
`headless_main.cpp` contains these items:

- the parser for the arguments
- the `SIGINT` and `SIGTERM` handler (the handler only writes a
  `volatile sig_atomic_t`)
- the incremental output of the transcript and the activity feed
- the exit-code policy in [Run Pig Pen](running.md#exit-codes)

## Build layout

`CMakeLists.txt` describes the Pig Pen targets. `cmake/dependencies.cmake` pins
and fetches the runtime dependencies. `cmake/testing.cmake` adds Catch2 and the
test suite. The library layout is the same as the Scry layout, which divides a
C++23 kernel without reflection from the C++26 code around it.

| library | standard | sources |
|---|---|---|
| `pigpen_world` | C++23 | `src/world` |
| `pigpen_core` | C++23 | `src/core` |
| `pigpen_agent` | C++26 + `-freflection` | `src/agent` |
| `pigpen_ui` (GUI or tests) | C++23 | `gui_options.cpp` and `world_animation.cpp`. It links only `pigpen_core`. |

`pigpen_core` never links `scry::scry`. Because of this, a `<scry/...>` include
in `pigpen_core` does not compile, directly or through `tool_contract.hpp`. The
linkage enforces the boundary, not a convention.

`pigpen_agent` links Scry privately and requests C++26 privately. Because of
this, the C++26 standard and `-freflection` do not go to the targets that link
`pigpen_agent`. Both entry points, `pigpen_ui`, and `pigpen_tests` compile as
C++23. Only `pigpen_agent` and `pigpen_reflection_tests` compile with
reflection. For this reason, the public agent headers (`session.hpp`,
`metrics_writer.hpp`) stay C++23-parseable.

Put code in `pigpen_agent` only when reflection replaces hand-written shape
code there. Examples are the tool contract and toolbox, the transport, and the
reflected JSONL records. The metrics writer encodes those records with the Scry
codec, so the runtime has no other JSON library. nlohmann/json is a dependency
only for the tests. The tests use it to parse logs and manifests independently.

The `pigpen_target()` helper applies C++23 and the warning flags only to the
Pig Pen targets. The fetched dependencies are `SYSTEM`. For more data, refer to
[Build Pig Pen](building.md).
