# Architecture

Pig Pen is a C++23 application. It joins two other projects:

- [scrydantic](https://github.com/crybo-rybo/scrydantic) gives the
  model-facing type system. It makes the tool schemas, the strict argument
  decoder, the result encoder, and the tool bindings from the C++
  declarations, with C++26 reflection.
- [llamad](https://github.com/crybo-rybo/llamad) is the only model backend. It
  is a gRPC daemon on a Unix socket. It serves one llama.cpp model.

Neither project runs an agent loop. llamad keeps no conversation and never
runs a tool. scrydantic only binds and codes tools. Thus, Pig Pen owns the
loop: the conversation history, the tool rounds, the call budget, and the
round limit.

Only `src/agent` uses C++26 reflection. Only `src/llamad` uses gRPC. With
these layers, you can test all code without a window or a model.

```
src/app/main.cpp              src/app/headless_main.cpp
        │  SDL3 + ImGui loop          │  argv parsing, signals, stdout
        ▼                             ▼
   src/ui/  ─────────────►  agent::Session
   AppUi, WorldAnimation          │
                                  ├── core::EpisodeRunner      turn loop, play/pause/stop
                                  ├── core::ToolLoopTransport  tool rounds, budgets, history
                                  │     ├── core::IChatClient     ◄── llamad::LlamadChatClient ──► gRPC ──► llamad
                                  │     └── core::IToolDispatcher ◄── agent::WorldToolDispatcher (scrydantic)
                                  ├── core::WorldTools         typed actions and visibility
                                  ├── MetricsWriter            reflected JSONL
                                  └── world::World             the simulation
```

## `src/world`: the simulation

`World` is a value type. It contains the seed, the blob position, the score, a
flat item array, an observed bitset, and the eaten count for each item. It does
not use JSON, networks, or callbacks. Actions return typed results
(`MoveResult`, `LookResult`, `EatResult`) with enumerated failures, not
strings. `dump()` gives a canonical serialization for the determinism tests.
For more data, refer to [World and tools](world.md).

## `src/core`: the runtime without reflection or gRPC

This layer is plain C++23 in the namespace `pigpen::core`. The build makes it
as `pigpen_core`. Code in this layer cannot include a scrydantic header or a
gRPC header.

| unit | responsibility |
|---|---|
| `config.hpp` | `Config`: the llamad socket, the seed, the budgets, the sampling settings, and the three model-visibility flags. Both front ends use it. `default_socket_path()` applies the llamad default rule. |
| `prompt.cpp` | Creates the system prompt from `Config`. Creates the instructions for each turn from the feedback and the optional human guidance. |
| `tool_responses.hpp` | Flat response aggregates, with the world outcome fields. scrydantic reflects over them in `src/agent`. |
| `world_tools.cpp` | Typed world actions and scenario visibility. It takes world values. It contains no JSON, reflection, budgets, or schema code. |
| `events.hpp` | `ToolActivity` and its append-only feed. Each item has typed application semantics, the call identity, and the exact canonical argument text and result text. |
| `turn_transport.hpp` | `ITurnTransport`, the interface for an implementation that sends one turn and gets callbacks. |
| `chat_client.hpp` | `IChatClient`, the interface for one chat request, and plain value types that mirror the llamad wire contract. It contains no JSON code. |
| `tool_dispatcher.hpp` | `IToolDispatcher`, the interface that runs one tool call and gives the exact text for the model. |
| `tool_loop.cpp` | `ToolLoopTransport`, the real `ITurnTransport`. It runs the tool rounds of one turn over an `IChatClient` and an `IToolDispatcher`. |
| `episode_runner.cpp` | The state machine (`idle → playing ⇄ paused → finished`), the turn budget, cooperative cancellation, the transcript, and observers for turn completion and episode completion. |

### The tool loop

llamad is stateless. Each request carries the full conversation and the
tools. Thus, `ToolLoopTransport` keeps the committed history. The system
message is first. One turn does these steps:

1. It stages the user message. It starts round 1 with the committed history,
   the staged messages, all tools, and the sampling settings.
2. It sends each streamed text chunk to the runner, and adds it to the turn
   text.
3. When a request ends, it adds the token counts to the turn totals. Then it
   examines the finish reason:
   - **Tool calls, below the round limit.** It stages the assistant message
     with the calls. It examines each call in sequence. If the turn already
     used its four requests, the call does not run, and the model gets the
     tool error for the call limit. Otherwise the call uses one request of the
     budget, also if it is not valid, and the dispatcher runs it. The loop
     stages one `tool` message for each call, with the call ID. Then it starts
     the next round.
   - **Tool calls at the round limit.** This is the round-limit completion.
     The calls do not run. The history keeps the text of the reply, if it is
     not empty, without the calls. Thus, the history never holds a call
     without an answer. The turn completes, and the outcome records the
     number of calls that did not run.
   - **End of generation, or a stop string.** The turn completes.
   - **Length.** The turn ends with an error, because the response got to its
     output-token limit.
   - **Cancelled.** The turn ends as cancelled.
   - **Error.** The turn ends with the llamad status and message.
4. The history is transactional. The loop commits the staged messages only
   when the turn completes. This includes the round-limit completion. A turn
   that fails or is cancelled discards them. The world side effects of the
   calls that ran stay, and the log records them.
5. If a later round cannot start, the turn ends with an error through
   `on_finished`.

The loop counts the requests of the turn, the dispatched rounds, all calls in
these rounds, the rejected calls (budget and admission), and the invalid
calls (unknown tools and arguments that do not decode). These counts are
always known, also for a turn that fails. `EpisodeRunner` sends them to the
metrics writer. In the prompt of the next turn, it also tells the model about
the calls that did not run.

## `src/llamad`: the gRPC client

This layer is C++23 in the namespace `pigpen::llamad`. The build makes it as
`pigpen_llamad`. It is the only Pig Pen code that includes gRPC and Protobuf
headers. Its public header, `llamad_client.hpp`, is free of gRPC.

The build gets `proto/llamad/v1/llamad.proto` from the pinned llamad release,
and generates the C++ messages and the gRPC stubs into
`pigpen_llamad_proto`. The generated code compiles without the Pig Pen warning
flags, and its headers are system headers.

`LlamadChatClient` implements `core::IChatClient`:

- `connect()` opens `unix:<socket path>`. It calls `GetModelInfo` with a time
  limit, and does not wait for the daemon. Thus, a missing socket fails
  immediately. `connect()` refuses an embedding model and a model without a
  chat template.
- `start()` sends one `Chat` request. Each request reads its stream on its
  own worker thread. The worker only puts text and terminal events into a
  queue that a mutex protects. It puts the terminal event last, after the
  stream finished.
- `pump()` delivers the queued events on the thread that calls it, with a
  maximum number of callbacks in each call.
- `cancel()` calls `ClientContext::TryCancel`. It is safe from the pump
  thread.
- The destructor cancels the active request, joins the worker, and drops the
  events that it did not deliver.

## `src/agent`: the scrydantic integration

This layer is C++26 with reflection, in the namespace `pigpen::agent`. The
build makes it as `pigpen_agent`. Put code here only when reflection replaces
hand-written shape code. Keep the public headers of this layer
C++23-parseable, so that the front ends can include them.

| unit | responsibility |
|---|---|
| `tool_contract.hpp` | The annotated `DirectionArguments`, the error result type, and the checks that all argument types and response types are reflectable. This file and `core/tool_responses.hpp` together are the contract with the model. |
| `world_tool_binding.cpp` | The scrydantic toolbox (`move`, `look`, `eat`) over `core::WorldTools`, and `WorldToolDispatcher`: the admission policy, the tool lookup, and the publication of each world transition with its exact payloads. |
| `metrics_writer.cpp` | The JSONL header, tool, turn, and footer records as reflected records. The writer always writes a footer, also after an abnormal shutdown. |
| `session.cpp` | Puts all of the above units into one owned object. |

Three seams make the layers testable:

- With `ITurnTransport`, a scripted transport in
  `tests/episode_runner_tests.cpp` can operate `EpisodeRunner`. The tests use
  this seam to cover the full turn loop without a model. This includes a stop
  that cancels an active turn.
- With `IChatClient` and `IToolDispatcher`, a scripted chat client in
  `tests/tool_loop_tests.cpp` can operate `ToolLoopTransport`. The tests use
  this seam to cover the request contents, the budget, the round limit, the
  transactional history, failures, and cancellation.
- `WorldTools` takes world values and returns the plain response types. The
  tests use this seam to examine world behavior, fixed response shapes, and
  scenario visibility without reflection, JSON, or a binding.

A scripted fake llamad (`tests/support/fake_llamad.cpp`) is a real gRPC
server on a temporary Unix socket. The tests use it to test the real client,
a full `Session`, and the CLI process.

### Reflection is the tool boundary

`WorldToolBinding` is a scrydantic toolbox. Its `move`, `look`, and `eat`
member functions have `scrydantic::tool` annotations.
`WorldToolDispatcher` calls `scrydantic::bind` one time with a
`std::shared_ptr` to the binding. Each bound handler shares the ownership of
the binding. scrydantic then does these steps for each tool:

- It gives the tool the name of its function.
- At compile time, it makes a closed JSON Schema from the parameters
  (`DirectionArguments`, or no parameters for `eat`).
- It strictly decodes the arguments.
- It calls the member function.
- It encodes the response.

The identifiers of the scoped enums supply the JSON strings from the same C++
declaration.

Each member function takes a `core::ToolCallContext` as its first parameter.
This context holds the round, the batch index, and the call ID from the tool
loop. The member runs the typed world action. It stages the world transition
with the canonical argument text. Then `dispatch()` adds the exact result
text that goes back to the model, and publishes the `ToolActivity` through
`on_activity`. Dispatch is one synchronous call on the pump thread. Thus,
each decoded call makes exactly one activity, and the log never needs to
encode a payload a second time.

`dispatch()` does these steps in sequence:

1. **Admission.** After objective completion or a log failure, the
   dispatcher refuses all world actions. This includes the subsequent calls
   in the same batch. A log failure ends the episode after the active turn
   ends. If the turn completes, the results of the executed calls can commit.
2. **Lookup.** An unknown tool name is not valid. The model gets the list of
   the registered tools.
3. **Decode and call.** Arguments that do not decode are not valid. The model
   gets the scrydantic `model_message`.

scrydantic encodes all error results as `{"error": ...}`. Pig Pen never puts
model text into JSON by hand. Refused calls and calls that are not valid
never become world activity.

### `Session` is the reset unit

A `Session` owns the world, the llamad connection, the tool loop with its
conversation, the bound world tools, the runner, and the metrics writer. A
partial reset is not possible. To start again, destroy the session and create
a new one. The GUI **Reset** button does exactly this. For this reason,
changes to the connection and the scenario do not change an active episode.
The GUI shows "Pending settings apply on Reset" instead.

`Session::create()` does these steps in sequence:

1. It validates the config.
2. It connects to llamad.
3. It makes the world and the tool binding.
4. It opens the log last.

Thus, a config that is not valid, or a daemon that Pig Pen cannot reach,
leaves no log.

The members are in an order that destroys the runner first, then the tool
loop, then the client, then the tools, and then the world. The runner and the
tool loop cancel the active request. The client joins its worker and drops
the events that it did not deliver. No state is destroyed while a callback
can still reach it.

Clients examine the runner through a const view, and control it through
`Session`.

### The front ends pump the session

`Session::pump()` delivers a maximum of 32 queued model events. Then it ticks
the runner. Both front ends call `pump()` from their own loop:

- The GUI calls it one time for each frame.
- The CLI calls it in a tight loop. When there is no work, the loop sleeps for
  1 ms.

gRPC reads occur on a worker thread that only queues events. Application
callbacks and tools run only on the pump thread. Neither front end blocks to
wait for a result.

For the same reason, cancellation is cooperative. `stop()` asks the tool loop
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
and fetches the runtime dependencies, and finds gRPC and Protobuf.
`cmake/testing.cmake` adds Catch2, the fake llamad, and the test suite.

| library | standard | links | sources |
|---|---|---|---|
| `pigpen_world` | C++23 | — | `src/world` |
| `pigpen_core` | C++23 | world | `src/core` |
| `pigpen_llamad_proto` | C++17 or later | Protobuf, gRPC | the generated llamad stubs only |
| `pigpen_llamad` | C++23 | core (public); proto, gRPC (private) | `src/llamad` |
| `pigpen_agent` | C++26 + `-freflection` (private) | core (public); llamad, scrydantic (private) | `src/agent` |
| `pigpen_ui` (GUI or tests) | C++23 | core | `gui_options.cpp` and `world_animation.cpp` |

`pigpen_core` links neither `scrydantic::scrydantic` nor gRPC. Because of
this, a `<scrydantic/...>` or `<grpcpp/...>` include in `pigpen_core` does
not compile. The linkage enforces the boundary, not a convention.

`pigpen_agent` links scrydantic privately and requests C++26 privately.
Because of this, the C++26 standard and `-freflection` do not go to the
targets that link `pigpen_agent`. Both entry points, `pigpen_ui`, and
`pigpen_tests` compile as C++23. Only `pigpen_agent` and
`pigpen_reflection_tests` compile with reflection. For this reason, the
public agent headers (`session.hpp`, `metrics_writer.hpp`) stay
C++23-parseable.

Put code in `pigpen_agent` only when reflection replaces hand-written shape
code there. Examples are the tool contract and toolbox, and the reflected
JSONL records. The metrics writer encodes those records with the scrydantic
codec, so the runtime has no other JSON library. nlohmann/json is a dependency
only for the tests. The tests use it to parse logs and requests
independently.

The `pigpen_target()` helper applies C++23 and the warning flags only to the
Pig Pen targets. The fetched dependencies and the generated code are
`SYSTEM`. For more data, refer to [Build Pig Pen](building.md).
