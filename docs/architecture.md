# Architecture

Pig Pen is a C++23 application. It joins two other projects:

- [scrydantic](https://github.com/crybo-rybo/scrydantic) 0.2.0 gives the
  model-facing type system. It makes the tool schemas, the strict argument
  decoder, the result encoder, and the tool bindings from the C++
  declarations, with C++26 reflection.
- [llamad](https://github.com/crybo-rybo/llamad) 0.2.x is the only model
  backend. It is a gRPC daemon on a Unix socket. It serves one llama.cpp
  model.

Neither project runs an agent loop. llamad keeps no conversation and never
runs a tool. scrydantic only binds and codes tools. Thus, Pig Pen owns the
loop: the conversation history, the tool rounds, the call budget, and the
round limit.

Only `src/agent` uses C++26 reflection. In the application, only
`src/llamad` uses gRPC. The fake llamad of the tests also uses it.
`src/runtime` joins the layers into one `Session`, and uses neither. With
these layers, you can test all code without a window or a model.

```
src/app/main.cpp              src/app/headless_main.cpp
        │  SDL3 + ImGui loop          │  argv parsing, signals, stdout
        ▼                             ▼
   src/ui/  ─────────────►  runtime::Session
   AppUi, WorldAnimation          │
                                  ├── core::EpisodeRunner      turn loop, play/pause/stop
                                  ├── core::ToolLoopTransport  tool rounds, budgets, history
                                  │     ├── core::IChatClient     ◄── llamad::LlamadChatClient ──► gRPC ──► llamad
                                  │     └── core::IToolDispatcher ◄── agent::make_world_tool_dispatcher() (scrydantic)
                                  │           └── core::WorldTools, core::admission_refusal()
                                  ├── agent::MetricsWriter     reflected JSONL
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
as `pigpen_core`. Code in this layer cannot include a scrydantic header, and
must not include a gRPC header. The build enforces only the first rule. For
more data, refer to [Build layout](#build-layout).

| unit | responsibility |
|---|---|
| `config.hpp` | `Config`: the llamad socket, the seed, the budgets, the sampling settings, and the three model-visibility flags. Both front ends use it. `default_socket_path()` applies the llamad default rule. `estimated_context_tokens()` gives the rough context estimate for a turn budget. |
| `prompt.cpp` | Creates the system prompt from `Config`. Creates the instructions for each turn from the feedback and the optional human guidance. |
| `tool_responses.hpp` | Flat response aggregates, with the world outcome fields. scrydantic reflects over them in `src/agent`. |
| `world_tools.cpp` | Typed world actions and scenario visibility, and the admission policy for world actions (`admission_refusal()`). It takes world values. It contains no JSON, reflection, budgets, or schema code. |
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
   the staged messages, all tools, and the sampling settings. Each request
   asks for no reasoning (`ChatReasoning::off`).
2. It sends each streamed text chunk to the runner, and adds it to the turn
   text.
3. When a request ends with a finish, it adds the token counts and the daemon
   timings (`prompt_ms`, `completion_ms`) to the turn totals. Then it examines
   the finish reason:
   - **Tool calls, below the round limit.** It stages the assistant message
     with the calls. It examines each call in sequence. If the turn already
     used its four requests, the call does not run, and the model gets the
     tool error for the call limit. Otherwise the call uses one request of the
     budget, also if it is not valid, and the dispatcher runs it. The loop
     stages one `tool` message for each call, with the call ID. The round is
     then answered. If a cancel occurred during the round, the turn ends as
     cancelled. Otherwise the loop starts the next round.
   - **Tool calls at the round limit.** This is the round-limit completion.
     The calls do not run. The history keeps an assistant message with the
     text of the reply, without the calls. The text can be empty. Thus, the
     history never holds a call without an answer. The turn completes, and
     the outcome records the number of calls that did not run.
   - **End of generation, or a stop string.** The turn completes. The loop
     stages an assistant message with the text of the reply. The text can be
     empty.
   - **Length.** llamad uses this finish reason for two causes: the reply
     used its output-token limit, or the context has no space for another
     token. `ToolLoopLimits::context_tokens` (the `n_ctx` of the daemon)
     separates them. If the prompt tokens plus the output tokens of this
     request are `context_tokens` or more, the turn ends with
     `context_exhausted`, and the error gives the three numbers. Otherwise
     the turn ends with an error that gives the output-token limit and the
     output tokens of the reply. If `context_tokens` is `0` (unknown), the
     turn ends with an error that names both causes.

   A request without a finish ends the turn. The `ChatErrorKind` of the chat
   client gives the status:
   - After a cancel, the turn ends as cancelled: llamad ends a cancelled
     stream with the status `CANCELLED` and no finish. A cancel has priority
     over the other kinds.
   - `prompt_too_long` (llamad `OUT_OF_RANGE`: the prompt is longer than the
     context) ends the turn with `context_exhausted`, with the llamad
     message.
   - Otherwise the turn ends with an error, with the llamad status and
     message.

   The loop never trims or summarizes the history. A turn that ends with
   `context_exhausted` ends the episode with the same finish reason.
4. The history always matches the world. The calls of an answered round
   ran, and their side effects stay. Thus, the loop always commits each
   answered round:
   - A completed turn commits all its staged messages. This includes the
     round-limit completion. The turn always ends with an assistant message,
     so the roles alternate on the next turn.
   - A turn that fails (also with `context_exhausted`) or is cancelled
     commits the user message and the answered rounds. It discards only the reply after the last answered
     round.
   - A turn that fails before it answers a round commits nothing. Thus, the
     history never holds a `user` message without a reply.
   - The log records the side effects of all calls that ran.
5. If a later round cannot start, the turn ends with an error through
   `on_finished`. The answered rounds stay in the history.
6. Each `cancel()` during a turn goes to the chat client. Only the first
   `cancel()` of a turn returns `true`. A cancel from inside a tool call
   does not stop the round. The round gets all its answers, and the next
   request does not start.

`Session` (in `src/runtime`) gives the `n_ctx` that llamad reports at connect to
`ToolLoopLimits::context_tokens`. It also compares `n_ctx` with
`core::estimated_context_tokens()` for the turn budget. If `n_ctx` is
smaller, `Session::startup_warning()` returns a warning, and both front ends
show it. The warning never stops the session.

The loop counts the requests of the turn, the dispatched rounds, all calls in
these rounds, the rejected calls (budget and admission), and the invalid
calls (unknown tools and arguments that do not decode). These counts are
always known, also for a turn that fails. `EpisodeRunner` sends them to the
metrics writer. In the prompt of the next turn, it also tells the model about
the calls that did not run.

## `src/llamad`: the gRPC client

This layer is C++23 in the namespace `pigpen::llamad`. The build makes it as
`pigpen_llamad`. It is the only application code that includes gRPC and
Protobuf headers. The fake llamad of the tests also includes them. Its public
header, `llamad_client.hpp`, is free of gRPC.

The build gets `proto/llamad/v1/llamad.proto` from the pinned llamad commit
(version 0.2.0), and generates the C++ messages and the gRPC stubs into
`pigpen_llamad_proto`. The generated code compiles without the Pig Pen warning
flags, and its headers are system headers.

`LlamadChatClient` implements `core::IChatClient`:

- `connect()` opens `unix:<socket path>`. It calls `GetModelInfo` with a time
  limit (`ConnectOptions::deadline`, which the caller gives), and does not
  wait for the daemon. Thus, a missing socket fails immediately. A process
  that accepts the connection but does not answer fails at the time limit,
  and the message names the limit. `connect()` refuses a daemon that is not llamad 0.2.x, because
  before 1.0 a minor release can change the contract. llamad 0.1.x reports no
  version. The required series is one constant, `required_llamad_series`. It
  changes with the pin. `connect()` also refuses an embedding model and a
  model without a chat template.
- `ModelInfo` has the model description, the model name and file, the
  context sizes, the llamad version, and the request size limit.
- `start()` sends one `Chat` request. Each request reads its stream on its
  own worker thread. The worker only puts text and terminal events into a
  queue that a mutex protects. It puts the terminal event last, after the
  stream finished. It skips `progress` chunks and chunk kinds that it does
  not know, but it records each of them as a sign of life.
- `pump()` first runs the stall watchdog. Then it delivers the queued events
  on the thread that calls it, with a maximum number of callbacks in each
  call.
- The stall watchdog uses the liveness contract of llamad: while a request
  queues or runs, the stream is silent for at most about one second longer
  than the slowest step of the engine. The worker records the time of the
  last sign of life: the start of the call, and each chunk that it reads.
  If the silence is longer than `ConnectOptions::stall_timeout` (from
  `Config::stall_timeout_seconds`, default 120 s, `0` is off), `pump()`
  cancels the call and marks the request as stalled. The request then ends
  through its normal terminal event, exactly one time, as a `failure` with
  the message `llamad sent nothing for N seconds, so Pig Pen stopped the
  request`. The watchdog measures silence, not the duration of the request.
  Thus, a request that waits in the queue behind another client is not
  stopped.
- A stream that ends without a finish gives a `ChatError`. Its kind is
  `failure` after a stall, `cancelled` after a local cancel,
  `prompt_too_long` for the status `OUT_OF_RANGE` (the prompt is longer than
  the context), and `failure` for all other statuses. The message is
  `llamad <STATUS>: <message>`, or the stall message. A local cancel before
  the stall has priority. A finish chunk that the daemon sent before the
  cancel or the stall took effect has priority over both.
- `cancel()` calls `ClientContext::TryCancel`. It is safe from the pump
  thread.
- The destructor cancels the active request, joins the worker, and drops the
  events that it did not deliver.

The threads use the client state in this way. All state that two threads
use is under one mutex:

| state | pump thread | worker thread |
|---|---|---|
| socket path, model info, stub, stall timeout | reads | reads (stub, stall timeout) |
| event queue | pops (`pump()`), clears (destructor) | pushes |
| context of the active call | sets (`start()`), resets (`pump()`, at the terminal event), cancels (`cancel()`, watchdog) | uses its own reference |
| cancel flag | resets (`start()`), sets (`cancel()`) | reads after the stream ends |
| stall flag | resets (`start()`), sets (watchdog, only if no cancel came first) | reads after the stream ends |
| time of the last sign of life | sets (`start()`), reads (watchdog) | sets (each chunk), clears (end of the stream) |
| callbacks, active flag, request ID, worker handle | reads and writes | — |

`connect()` writes the first row before the client exists. Then no thread
changes it, so it needs no lock.

## `src/agent`: the scrydantic integration

This layer is C++26 with reflection, in the namespace `pigpen::agent`. The
build makes it as `pigpen_agent`. Put code here only when reflection replaces
hand-written shape code. The layer has the tool contract, the toolbox and its
dispatcher, the factory of the dispatcher, and the metrics writer. It has no
composition code, and it does not link the llamad client.

Two headers of this layer are C++23-parseable, so that the C++23 layers can
include them: `world_tool_dispatcher.hpp` and `metrics_writer.hpp`.
`tool_contract.hpp` and `world_tool_binding.hpp` include scrydantic, so they
need C++26 reflection. Only the TUs of this layer and the reflection tests
include them.

| unit | responsibility |
|---|---|
| `tool_contract.hpp` | The annotated `DirectionArguments`, the error result type, and the checks that all argument types and response types are reflectable. This file and `core/tool_responses.hpp` together are the contract with the model. C++26 only. |
| `world_tool_dispatcher.hpp` | The C++23 entry point of the layer: `make_world_tool_dispatcher()`, which returns a `core::IToolDispatcher`, and `WorldToolObservers`, the callbacks of the owner. |
| `world_tool_binding.cpp` | The scrydantic toolbox (`move`, `look`, `eat`) over `core::WorldTools`, its call context `WorldToolCall`, and `WorldToolDispatcher`: it applies the admission policy, finds the tool, and publishes each world transition with its exact payloads. The declarations in `world_tool_binding.hpp` are C++26 only. |
| `metrics_writer.cpp` | The JSONL header, tool, turn, and footer records as reflected records. The writer always writes a footer, also after an abnormal shutdown. |

### Reflection is the tool boundary

`WorldToolBinding` is a scrydantic toolbox. Its `move`, `look`, and `eat`
member functions have `scrydantic::tool` annotations.
`WorldToolDispatcher` makes the binding, and calls `scrydantic::bind` one
time with a `std::shared_ptr` to it. Each bound handler shares the ownership
of the binding. scrydantic then does these steps for each tool:

- It gives the tool the name of its function.
- At compile time, it makes a closed JSON Schema from the parameters
  (`DirectionArguments`, or no parameters for `eat`).
- It strictly decodes the arguments.
- It calls the member function.
- It encodes the response.

The identifiers of the scoped enums supply the JSON strings from the same C++
declaration.

The binding uses `WorldToolCall` as its scrydantic call context. Each member
function takes a `const WorldToolCall &` as its first parameter. scrydantic
does not put the context into the schema. The context holds two items:

- `call`: the `core::ToolCallContext` from the tool loop, with the round, the
  batch index, and the call ID.
- `activity`: a reference to the output slot of the call, an empty
  `std::optional<core::ToolActivity>`.

For each call, `dispatch()` makes a new empty slot on its stack, and gives it
to the handler in the context. The member runs the typed world action. Then
it writes the world transition, with the canonical argument text, into the
slot. After the handler returns, `dispatch()` reads the slot. If the slot is
empty, the arguments did not decode, and no world action ran. Otherwise
`dispatch()` adds the exact result text that goes back to the model, and
publishes the `ToolActivity` through `WorldToolObservers::on_activity`.
Dispatch is one synchronous call on the pump thread. Thus, each decoded call
makes exactly one activity, and the log never needs to encode a payload a
second time.

The owner gives the observers to `make_world_tool_dispatcher()`. They cannot
change after that.

`dispatch()` does these steps in sequence:

1. **Admission.** The dispatcher polls `WorldToolObservers::logging_failed`,
   and asks `core::admission_refusal()`. After objective completion or a log
   failure, the policy refuses all world actions. This includes the
   subsequent calls in the same batch. A log failure ends the episode after
   the active turn ends. The results of the executed calls commit with their
   round.
2. **Lookup.** An unknown tool name is not valid. The model gets the list of
   the registered tools.
3. **Decode and call.** Arguments that do not decode are not valid. The model
   gets the scrydantic `model_message`.

scrydantic encodes all dispatcher error results as `{"error": ...}`. The
only other error result is the call-limit result of the tool loop. It is a
fixed string constant, `tool_call_limit_result` in `src/core/tool_loop.hpp`.
Pig Pen never puts model text into JSON by hand. Refused calls and calls that
are not valid never become world activity.

## `src/runtime`: the composition root

This layer is C++23 in the namespace `pigpen::runtime`. The build makes it as
`pigpen_runtime`. It has one unit, `session.cpp`. `Session` validates the
config, connects to llamad, and joins the world, the dispatcher, the tool
loop, the runner, and the log. `PumpStats` and `TokenTotals` are also in
`session.hpp`.

The layer links `pigpen_core` publicly, and `pigpen_llamad` and
`pigpen_agent` privately. It includes only C++23 headers of these layers:
`llamad/llamad_client.hpp`, `agent/world_tool_dispatcher.hpp`, and
`agent/metrics_writer.hpp`. Thus, it compiles without reflection, and
clang-tidy can analyse it. Its public header, `session.hpp`, includes only
`core` and `world` headers. Both front ends and `pigpen_tests` link
`pigpen_runtime`.

Three seams make the layers testable:

- With `ITurnTransport`, a scripted transport in
  `tests/episode_runner_tests.cpp` can operate `EpisodeRunner`. The tests use
  this seam to cover the full turn loop without a model. This includes a stop
  that cancels an active turn.
- With `IChatClient` and `IToolDispatcher`, a scripted chat client in
  `tests/tool_loop_tests.cpp` can operate `ToolLoopTransport`. The tests use
  this seam to cover the request contents, the budget, the round limit, the
  committed history after each type of turn end, failures, and cancellation.
- `WorldTools` and `admission_refusal()` take world values and return plain
  types. The tests use this seam to examine world behavior, fixed response
  shapes, scenario visibility, and the admission policy without reflection,
  JSON, or a binding.

The reflection tests make the real dispatcher with
`make_world_tool_dispatcher()`, and examine its results through its
observers.

A scripted fake llamad (`tests/support/fake_llamad.cpp`) is a real gRPC
server on a temporary Unix socket. The tests use it to test the real client,
a full `Session`, and the CLI process.

### `Session` is the reset unit

A `Session` owns the world, the llamad connection, the tool loop with its
conversation, the world tool dispatcher (with the bound tools), the runner,
and the metrics writer. A partial reset is not possible. To start again,
destroy the session and create a new one. The GUI **Reset** button does
exactly this. For this reason, changes to the connection and the scenario do
not change an active episode. The GUI shows "Pending settings apply on Reset"
instead.

`Session::create()` does these steps in sequence:

1. It validates the config.
2. It connects to llamad. This is the only step that blocks: one
   `GetModelInfo` call, with the deadline that the caller gives (default 5
   seconds). The CLI gives `min(5 s, --timeout-seconds)`, and starts its
   episode clock before this call. The GUI gives 1 second, because the
   frame loop waits.
3. It makes the world.
4. It opens the log.
5. It makes the dispatcher with `agent::make_world_tool_dispatcher()`, then
   the tool loop and the runner. These steps do not return an error.

The log is the last step that can return an error. Thus, a config that is
not valid, or a daemon that Pig Pen cannot reach, leaves no log.

`Session` holds the dispatcher as a `std::unique_ptr<core::IToolDispatcher>`.
The members are in an order that destroys the runner first, then the tool
loop, then the client, then the dispatcher with the tools, and then the
world. The runner and the tool loop cancel the active request. The client
joins its worker and drops the events that it did not deliver. No state is
destroyed while a callback can still reach it.

Clients examine the runner through a const view, and control it through
`Session`.

### The front ends pump the session

`Session::pump()` delivers a maximum of 32 queued model events. Then it ticks
the runner. Both front ends call `pump()` from their own loop:

- The GUI calls it one time for each frame.
- The CLI calls it in a tight loop. When there is no work, the loop sleeps for
  1 ms.

gRPC reads occur on a worker thread that only queues events. Application
callbacks and tools run only on the pump thread. During an episode, neither
front end blocks to wait for a result. The only blocking call is the
creation of the session: one handshake with a deadline. Refer to
[`Session` is the reset unit](#session-is-the-reset-unit). The stall watchdog
runs in `pump()`. Thus, a daemon that stops sending ends the turn with an
error, also when nobody pushes **Stop**.

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
  `volatile sig_atomic_t`). The handshake does not stop for a signal. A
  startup error after a signal exits with 128 + the signal.
- the incremental output of the transcript and the activity feed
- the exit-code policy in [Run Pig Pen](running.md#exit-codes), with exit
  code 6 for `context_exhausted`

## Build layout

`CMakeLists.txt` describes the Pig Pen targets. `cmake/dependencies.cmake` pins
and fetches the runtime dependencies, finds gRPC and Protobuf, and makes sure
that Pig Pen can link that gRPC.
`cmake/testing.cmake` adds Catch2, the fake llamad, and the test suite.

| library | standard | links | sources |
|---|---|---|---|
| `pigpen_world` | C++23 | — | `src/world` |
| `pigpen_core` | C++23 | world | `src/core` |
| `pigpen_llamad_proto` | C++17 or later | Protobuf, gRPC | the generated llamad stubs only |
| `pigpen_llamad` | C++23 | core (public); proto, gRPC (private) | `src/llamad` |
| `pigpen_agent` | C++26 + `-freflection` (private) | core (public); scrydantic (private) | `src/agent` |
| `pigpen_runtime` | C++23 | core (public); llamad, agent (private) | `src/runtime` |
| `pigpen_ui` (GUI or tests) | C++23 | core | `gui_options.cpp` and `world_animation.cpp` |
| `pigpen_test_support` (tests only) | C++23 | proto, gRPC (private) | `tests/support/fake_llamad.cpp`, the fake llamad |

`pigpen_core` links neither `scrydantic::scrydantic` nor gRPC. The build
fetches scrydantic, and its headers are only on the include path of the
targets that link it. Because of this, a `<scrydantic/...>` include in
`pigpen_core` does not compile. The linkage enforces this boundary. For gRPC,
the boundary is only a convention. When gRPC is a system package, for
example the Debian packages of the CI, its headers are in the default include
path. Thus, a `<grpcpp/...>` include in `pigpen_core` can compile.

`pigpen_agent` links scrydantic privately and requests C++26 privately.
Because of this, the C++26 standard and `-freflection` do not go to the
targets that link `pigpen_agent`. `pigpen_runtime`, both entry points,
`pigpen_ui`, and `pigpen_tests` compile as C++23. Only the TUs of
`pigpen_agent` and `pigpen_reflection_tests` compile with reflection. For
this reason, the agent headers that the C++23 targets include
(`world_tool_dispatcher.hpp`, `metrics_writer.hpp`) stay C++23-parseable.
The build proves it, because `src/runtime/session.cpp` and
`tests/metrics_writer_tests.cpp` include them. `tool_contract.hpp` and
`world_tool_binding.hpp` include scrydantic. A C++23 TU that includes them
does not compile, because scrydantic stops with an `#error` without
reflection.

Put code in `pigpen_agent` only when reflection replaces hand-written shape
code there. Examples are the tool contract and toolbox, and the reflected
JSONL records. Code without reflection goes to `pigpen_core` (policy, for
example the admission policy) or to `pigpen_runtime` (composition). The metrics writer encodes those records with the scrydantic
codec, so the runtime has no other JSON library. nlohmann/json is a dependency
only for the tests. The tests use it to parse logs and requests
independently.

The `pigpen_target()` helper applies C++23 and the warning flags only to the
Pig Pen targets. The fetched dependencies and the generated code are
`SYSTEM`. For more data, refer to [Build Pig Pen](building.md).
