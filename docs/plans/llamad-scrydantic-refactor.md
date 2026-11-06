# Pig Pen on llamad + Scrydantic: first-principles refactor plan

- **Repos:** pig-pen `~/Work/pig-pen` · llamad `~/Work/llamad` (v0.1.0, `c77ae5b`) · scrydantic `~/Work/scrydantic` (v0.2.0, `33ec842`)
- **Written:** 2026-11-06 · **Status:** approved 2026-11-06; implementation runs as four stacked PRs (§12).
- **Supersedes:** `MIGRATION_PLAN.md` (2026-09-20). That plan targeted llamad's C++ client library, which llamad removed on 2026-10-01 (PR #33 "make the proto the product; drop the client and C++26 reflection"). The reflection half of that client became Scrydantic.

## 1. What changed since September

| | September 2026 | Today |
|---|---|---|
| llamad | daemon + reflected C++ client (`llamad::client`), unreleased | daemon only, v0.1.0 released. **The proto is the only interface.** No client library in any language. KV prefix cache landed (resent history decodes only the new turn). Issues #3/#4/#5/#6 closed. |
| scry | HTTP/SSE harness, pig-pen pinned v0.7.0 | still exists; pig-pen is its only consumer. Its reflection codec was extracted into Scrydantic. |
| scrydantic | did not exist | v0.2.0 (released today): schema/encode/decode/tool binding, header-only C++26 on a C++23 JSON core, zero deps, MIT, FetchContent + `find_package`. |
| toolchain | GCC 16 snapshots | Arch ships GCC 16.2.1, gRPC 1.84, protobuf 36.1; `g++` is already 16.2 locally. Scrydantic requires 16.2+. |

Consequence: pig-pen must own **everything above the wire**: the gRPC stream, the tool loop, budgets, history, cancellation. llamad's docs say so explicitly ("the application runs the tools, does the loop over tool rounds"). Scrydantic replaces exactly the part of scry that was never transport: schemas, strict decode, canonical encode, and the toolbox binding.

### Spike (done, in the session scratchpad)

A throwaway CMake project proved the three load-bearing assumptions on this machine (GCC 16.2.1, CMake 4.4, Arch gRPC/protobuf):

1. `FetchContent_Declare(llamad GIT_TAG v0.1.0 GIT_SUBMODULES "" SOURCE_SUBDIR proto)` configures and builds `llamad_proto` **without llama.cpp**, provided the parent calls `find_package(Protobuf)`/`find_package(gRPC)` first. The generated stubs compile under `-std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. llamad issue #18 is therefore not a blocker.
2. gRPC sync `ClientReader` + `ClientContext::TryCancel()` from another thread: a blocked `Chat` against a hanging fake daemon returned `CANCELLED` 100 ms after the cancel, with zero chunks. The client does not need daemon cooperation (llamad #25) to honour SIGINT or GUI Reset promptly.
3. `scrydantic::bind<CallContext>(std::shared_ptr<WorldToolbox>)` with `move(const CallContext&, DirectionArguments)`, `look(...)`, and no-argument `eat(const CallContext&)` returning `Result<EatToolResponse>`:
   - schemas are **byte-identical** to the scry ones pig-pen's tests and docs pin today (`additionalProperties:false`, enum from `world::Direction`, `eat` = empty closed object);
   - `tool_error("All positive-value items have been eaten.")` reaches `model_message`; a bad enum gives `$.direction is not a declared enumerator; must be one of: north, south, east, west` (same text as today); an extra key gives `$ contains unknown member "x"`;
   - a `std::variant` of tagged log records with `scrydantic::Json` splices and `std::string_view` members encodes to the same JSONL shape the metrics writer emits today; `JsonView` reads it back.

## 2. Target architecture

```
src/app/main.cpp                 src/app/headless_main.cpp
      │ SDL3 + ImGui frame loop        │ argv, signals, stdout, exit codes
      ▼                                ▼
 src/ui ───────────────►  agent::Session  ◄──────────────   (C++26, pigpen_agent)
                              │   composition root + reset unit
                              ├── core::EpisodeRunner        turn budget, play/pause/stop   (C++23)
                              ├── core::ToolTurnTransport    the tool loop: rounds, call budget,
                              │        │                     history, worker thread, cancel      (C++23)
                              │        ├── core::IChatClient ◄── llamad::LlamadChatClient  gRPC Chat/GetModelInfo (C++23, pigpen_llamad)
                              │        └── core::ToolTable   name → opaque JSON handler          (C++23)
                              │                 ▲
                              ├── agent::WorldToolbox  scrydantic::bind over core::WorldTools   (C++26)
                              ├── agent::MetricsWriter scrydantic::encode of reflected records  (C++26)
                              └── world::World                                                   (C++23)
```

### Libraries and standards

| target | std | contents | links |
|---|---|---|---|
| `pigpen_world` | C++23 | unchanged | |
| `pigpen_core` | C++23 | config, prompt, world_tools, tool_responses, events, episode_runner, **chat_client.hpp** (interface + plain types), **tool_table.hpp**, **tool_turn_transport.{hpp,cpp}** | `pigpen_world`, `scrydantic::json` (the C++23 JSON core: `escape_json_string`, `JsonView`; no reflection) |
| `pigpen_llamad` | C++23 | `llamad_chat_client.{hpp,cpp}`: `core::IChatClient` over the generated stub. Header exposes no gRPC type. | `pigpen_core` PUBLIC, `llamad_proto` PRIVATE |
| `pigpen_agent` | C++26 `-freflection` | `tool_contract.hpp`, `world_toolbox.{hpp,cpp}`, `metrics_writer.{hpp,cpp}`, `session.{hpp,cpp}` | `pigpen_core` PUBLIC, `pigpen_llamad` + `scrydantic::scrydantic` PRIVATE |
| `pigpen_ui`, `pig-pen`, `pig-pen-headless`, `pigpen_tests` | C++23 | as today | |
| `pigpen_reflection_tests` | C++26 | toolbox + contract tests | `pigpen_agent`, `scrydantic::scrydantic` |

Invariants carried forward: reflection lives only in `pigpen_agent`; `pigpen_core` links neither `scrydantic::scrydantic` (the reflected codec) nor gRPC, so the loop, budgets, and history are testable with fakes and no model. It does link `scrydantic::json`, the reflection-free C++23 core, for `{"error":...}` replies and canonical JSON. New invariant: **gRPC lives only in `pigpen_llamad`**, so `scrydantic` and `grpcpp` headers never meet in one TU, and `clang-tidy` can now cover the whole tool loop and the gRPC client (both C++23).

### The three seams

1. `core::ITurnTransport` (runner ↔ loop): unchanged shape, `send(user_message, callbacks)` + `cancel()`. One terminal callback per accepted send, delivered on the pump thread.
2. `core::IChatClient` (loop ↔ daemon): **blocking, one round**:
   ```cpp
   struct ChatMessage { std::string role, content; std::vector<ToolCall> tool_calls; std::string tool_call_id; };
   struct ToolSpec    { std::string name, description, parameters_json_schema; };
   struct Sampling    { double temperature; std::uint32_t max_tokens; std::optional<std::uint32_t> seed; };
   struct RoundStats  { std::int32_t prompt_tokens, completion_tokens, cached_prompt_tokens; double prompt_ms, completion_ms; };
   struct ChatRound   { std::string text; RoundFinish finish /*eog,length,stop,tool_calls*/; std::vector<ToolCall> tool_calls; RoundStats stats; };
   struct ChatError   { enum class Kind { cancelled, deadline, unreachable, invalid_request, unsupported_model, failed } kind; std::string message; };
   struct ModelInfo   { std::string description; std::uint32_t n_ctx, n_ctx_train; bool has_chat_template, serves_embeddings; };

   class IChatClient {
     virtual std::expected<ModelInfo, ChatError> model_info(std::chrono::milliseconds timeout) = 0;
     virtual std::expected<ChatRound, ChatError> chat(const ChatRequest&, std::stop_token stop,
                                                      std::function<void(std::string_view)> on_text) = 0;
   };
   ```
   Blocking makes the fake trivial (scripted rounds, optional block-until-cancel) and puts all threading in one place.
3. `core::ToolTable` (loop ↔ reflected toolbox): a vector of `{name, description, schema_json, std::move_only_function<std::expected<std::string /*result json*/, std::string /*model-facing error*/>(const CallContext&, std::string_view args_json)>}`. The agent layer fills it from `scrydantic::BoundTool<CallContext>` with a five-line adapter; tests fill it with lambdas. `scrydantic::BoundTool` itself cannot cross into C++23 because its header includes `<meta>`.

## 3. Component designs

### 3.1 Config, CLI, GUI

`core::Config`: replace `base_url` + `model` with `std::string socket` (default = llamad's own default: `$XDG_RUNTIME_DIR/llamad.sock`, else `/tmp/llamad-<uid>.sock`, computed at startup so the two programs agree). Keep `seed`, `turn_budget`, `max_tool_rounds`, `max_output_tokens` (→ `SamplingParams.max_tokens`), `temperature` (→ float), `sampling_seed` (→ `SamplingParams.seed`, now honoured in-process by llama.cpp's sampler instead of best-effort by a server), and the three visibility flags. Validation: socket non-empty and < 108 bytes (`sun_path`).

CLI: `--socket PATH` replaces `--base-url`/`--model`; `PIGPEN_API_KEY` gone. Startup line prints `model="<ModelInfo.description>" socket=... context_tokens=N`. Exit codes unchanged; a daemon that is down or serves an embedding model is exit 1 **before** any log directory is created.

GUI: Connection section = **Socket** text field + read-only model description/context after a successful probe. `--socket` flag. Because there is no longer a required `--model` to imply intent, auto-start becomes an explicit `--play` flag (decision 1 below). Reset with the daemon down shows the probe error within the probe timeout (2 s) and keeps the controls usable.

### 3.2 `pigpen_llamad::LlamadChatClient`

~150 lines. Owns a `grpc::Channel` (`unix:` + socket, insecure) and a `Llama::Stub`.
- `model_info(timeout)`: `ClientContext::set_deadline`; maps `UNAVAILABLE` → `unreachable` with a message naming the socket and the `llamad --model … --socket …` command; `serves_embeddings`/`!has_chat_template` → `unsupported_model` (checked by Session).
- `chat(...)`: builds `ChatRequest` field-by-field (messages incl. replayed `tool_calls` and `tool_call_id`, tools, sampling with `optional` setters only when set), opens the stream, `std::stop_callback` → `TryCancel()`, loops `Read()`: text → `on_text` (called on the worker thread; the loop marshals), finish → captured. `Finish()` status mapping: `CANCELLED` → `cancelled`, `DEADLINE_EXCEEDED` → `deadline`, `UNAVAILABLE` → `unreachable`, `INVALID_ARGUMENT` → `invalid_request` (includes "prompt too long", llamad #21), `FAILED_PRECONDITION` → `unsupported_model`, else `failed`. A stream that ends without a `finish` chunk and OK status is a contract violation → `failed`. Nothing throws across the boundary.
- No reasoning toggle exists in llamad (scry had `ReasoningMode::disabled`); see issue draft A.

### 3.3 `core::ToolTurnTransport` (the tool loop)

Replaces `ScryTurnTransport`, `scry::Harness`, `scry::Conversation`, and the admission/observation callbacks. Owns the conversation history (system prompt + committed turns).

`send(user_message, callbacks)`:
1. Refuse if a turn is live. Snapshot `history.size()` for rollback. Append user message. Start a `std::jthread` that runs rounds and pushes events (`TextDelta`, `RoundDone(expected<ChatRound,ChatError>)`) into a mutex-guarded deque.
2. `poll()` (pump thread, from `Session::pump()`): drain up to 32 events. Deltas → `on_text_delta`. `RoundDone`:
   - error → outcome `cancelled` (if `Kind::cancelled`) or `error` with the message; **roll history back** to the snapshot (same transactional semantics as scry).
   - `eog`/`stop` → append assistant message; outcome `completed`.
   - `length` → outcome `error` "model response reached its output-token limit" (existing behaviour; keep).
   - `tool_calls` → append assistant message **with the calls as received**; for each call in order: dispatch (3.4) → append `tool` message `{tool_call_id, content}`. If `stop_requested` → outcome `cancelled`, rollback. If `rounds == max_tool_rounds` → outcome `completed` with history kept, `unexecuted = calls not dispatched` (none; llamad delivers whole rounds, so "unexecuted" now means calls refused for the round limit: dispatch is skipped for all calls of a round that would start round `max+1`; the loop sends no further request and completes the turn, matching `ToolRoundLimitPolicy::complete`). Otherwise start the next round on a fresh worker.
3. Per-turn **call budget** `max_world_tool_calls_per_turn = 4` counts every requested call, valid or not, across rounds; call 5+ gets `{"error":"tool call limit for this turn reached; respond without calling tools"}` and is not dispatched.
4. `cancel()` → `stop_source.request_stop()`; between rounds the loop checks it. Destructor: request stop, join. Exactly one `on_finished` per send; none after destruction.
5. `TurnOutcome` gains: `requests` (Chat RPCs attempted this turn, replacing scry's `attempts`), summed `RoundStats`, and `ToolStats { rounds, requested, refused /*budget+admission*/, invalid /*unknown+decode*/, executed, round_limit_reached, unexecuted }`.

Text of a turn = concatenation of every round's visible text (streamed as it arrives). Timeouts: no per-round deadline by default (CPU prefill of a long history is legitimately slow; CLI keeps `--timeout-seconds`); `Config` gets no new knob now.

### 3.4 `agent::WorldToolbox` and dispatch

```cpp
struct CallContext { std::uint32_t round, index; std::string_view call_id; };  // the turn number is added by Session, which owns it
class WorldToolbox {              // bound with scrydantic::bind<CallContext>(shared_ptr)
  [[= scrydantic::tool{"Move one cell north, south, east, or west."}]]
  core::MoveToolResponse move(const CallContext&, DirectionArguments);
  [[= scrydantic::tool{"Scan every cell in one direction to the wall."}]]
  core::LookToolResponse look(const CallContext&, DirectionArguments);
  [[= scrydantic::tool{"Eat the item on the current cell, if present."}]]
  core::EatToolResponse eat(const CallContext&);
  std::optional<PendingTransition> take_pending();   // typed before/after/outcome staged by the member
};
```
`tool_contract.hpp` keeps `DirectionArguments` with `[[= scrydantic::description{...}]]`; the `static_assert`s become `scrydantic::ToolArguments<DirectionArguments>` and `scrydantic::Encodable<...Response>`. Descriptions and names stay verbatim, so the model-facing contract does not change.

Dispatch of one call (runs on the pump thread, in `Session`'s adapter that fills the `ToolTable`):
1. **Admission** (loop-level policy, before invoke): objective complete → `All positive-value items have been eaten. Summarize the completed episode without more tools.`; log failure → `World tools are unavailable because the episode log failed. Summarize the actions already taken.` Counted as `refused`.
2. Unknown name → `unknown tool "fly"; registered tools: eat, look, move`. Counted as `invalid`.
3. `bound.invoke(context, Json{arguments_json})`: decode failure → `error().model_message`; counted as `invalid`. Success → the member ran, `take_pending()` yields the typed transition; the `ToolActivity` is built **right here** with `arguments_json` (canonicalised via `JsonView::parse(...).to_json()` so logs compare line-for-line across runs) and `result_json = result->text` (the exact bytes sent back to the model). No second encode, no `observe()`/`flush_pending_activity()`, no `weak_ptr<Session>`, no `result_dispatched`.
4. Every error text is wrapped as `scrydantic::encode(ToolErrorReply{.error = text})` → `{"error":"..."}` (same shape the model sees today).

Scrydantic features used: `bind` with a context, `describe<WorldToolbox, CallContext>()` for a compile-time manifest check in tests, `tool_error`, `Error::model_message` vs `message`, `encode`, `JsonView`. Not used: constraints/validators (nothing to constrain), `skip_null` (keep today's explicit `null`s so the log format changes only where it must).

### 3.5 `agent::MetricsWriter` and the log format (v2)

Same reflected-record design, `scry::reflection::encode` → `scrydantic::encode`. Format changes, all driven by the new runtime, documented in `docs/logs.md`:

| record | change |
|---|---|
| `header` | `model` = `ModelInfo.description`; `base_url` → `socket`; add `context_tokens` (`n_ctx`) and `llamad_version` if issue B lands (else omit). |
| `tool` | drop `scry_turn_id`, `result_dispatched`, `action_executed` (always true). Keep `turn, tick, tool, call_id, round, index, args, result, before, after, score_after`. |
| `tool_error` **(new)** | `turn, round, index, call_id, tool, args (raw text as Json or null if not JSON), error, kind: refused\|unknown\|invalid`. Makes protocol failures observable for the first time; does **not** count toward exit 5 or `zero_tool_turn`. |
| `turn` | `attempts` → `requests`; `scry_tools` → `tools {rounds, requested, refused, invalid, executed, round_limit_reached, unexecuted}`; add `cached_prompt_tokens`, `prompt_ms`, `completion_ms` (sums over rounds). `input_tokens` = Σ prompt_tokens, `output_tokens` = Σ completion_tokens. |
| `footer` | unchanged. |

### 3.6 `agent::Session`

Members: `Config`, `ModelInfo`, `World`, `shared_ptr<WorldToolbox>` (scrydantic handlers co-own it), `ToolTable`, `unique_ptr<IChatClient>`, `ToolTurnTransport`, `EpisodeRunner`, `MetricsWriter`, `ToolActivityFeed`, `metrics_error`. `pump()` = `transport.poll(); runner.tick();` returning `PumpStats{events_delivered, events_remaining}`.

`create(config, log_dir, variant)`: validate → construct `LlamadChatClient` → `model_info(2s)` probe (unreachable / embedding / no template are errors; **no log directory is created on failure**) → world, toolbox, table → `MetricsWriter::create` (header from `ModelInfo`) → compose. A second overload `create(config, std::unique_ptr<IChatClient>, log_dir, variant)` is the test seam. Deleted: `register_tools`, `enable_shared_from_this`, `metrics_failure_pending`, `make_scry_config`, env lookup.

## 4. Tests

`pigpen_tests` (C++23, no reflection, no daemon):
- `tool_turn_transport_tests.cpp` with `FakeChatClient` (scripted rounds; `hang` blocks until the stop token fires): single round text; two tool rounds; budget across rounds (5th call refused, history shows it); unknown tool; round-limit completes with history kept; `length` → error; chat error → rollback; cancel mid-RPC and between rounds; destruction with a round in flight; exactly one outcome; token and stats sums; `requests` counts.
- `llamad_chat_client_tests.cpp`: an in-process `grpc::Server` on a temp Unix socket running `FakeLlamaService` (links `llamad_proto`): model_info; text + tool-call stream round-trips every field; each status code → `ChatError::Kind`; stop token cancels a hanging stream promptly; deadline; stream without `finish`.
- `session_tests.cpp` with an injected fake client: socket validation, probe failure leaves no directory, header fields.
- existing runner/prompt/world/animation/gui_options tests updated for the renamed fields.

`pigpen_reflection_tests` (C++26): `world_toolbox_tests.cpp`: `describe<>` manifest equals the three expected `(name, description, schema)` triples; invoke paths (success, enum error text, unknown member, `eat {}`), context plumbing, `take_pending()` semantics; `metrics_writer_tests.cpp` stays in `pigpen_tests` (header is C++23).

`pigpen_fake_llamad` (test executable): `--socket P --scenario FILE --record FILE`; serves `GetModelInfo` and scripted `Chat` rounds (`text`, `finish`, `tool_calls`, `stats`, `hang`, `status{code,message}`), records every `ChatRequest` as JSON. Written against `llamad_proto` and `scrydantic` (scenario and record types are reflected structs, so the fake needs no third JSON library). It is also what the C++ client tests instantiate in-process.

Python (`tests/headless_integration_tests.py`, `tests/headless_signal_tests.py`): spawn the fake daemon, run the CLI with `--socket`, assert the recorded request carries the **exact** tools array (names, descriptions, schema strings), the tool message content, `seed` only when set, stdout lines, exit 0 / 5, the JSONL log (incl. the new `tool_error` record), and for signals: `hang` scenario → exit 128+sig, cancelled turn, complete footer. No HTTP, no curl.

Test-side dependency: **drop nlohmann/json**; C++ tests read logs with `scrydantic::JsonView` through a 10-line `json_at(view, "a.b[0]")` helper (decision 3).

Safety: every CTest invocation passes an explicit throwaway `--socket`; the option-rejection tests use `--socket /nonexistent/pigpen.sock`. A bare `pig-pen-headless` would otherwise reach a developer's live daemon.

## 5. Build and CI

`cmake/dependencies.cmake`: delete all `SCRY_*`/`PIGPEN_SCRY_SOURCE` code. Add `find_package(Protobuf CONFIG QUIET)` with module fallback and `find_package(gRPC CONFIG REQUIRED)` (same recipe as llamad), then
```cmake
FetchContent_Declare(llamad GIT_REPOSITORY https://github.com/crybo-rybo/llamad.git GIT_TAG v0.1.0
  GIT_SUBMODULES "" GIT_SHALLOW TRUE SOURCE_SUBDIR proto SYSTEM EXCLUDE_FROM_ALL)
FetchContent_Declare(scrydantic GIT_REPOSITORY https://github.com/crybo-rybo/scrydantic.git GIT_TAG v0.2.0
  SYSTEM EXCLUDE_FROM_ALL)
```
Local overrides are the built-in `FETCHCONTENT_SOURCE_DIR_LLAMAD` / `_SCRYDANTIC`. Compiler gate becomes GCC ≥ 16.2 (Scrydantic's floor). `.clang-format` include regex `scry` → `scrydantic|llamad|grpcpp|google`.

CI: `build-and-test` moves into the `mirror.gcr.io/library/archlinux:base-devel` container (what llamad uses): `pacman -Syu --noconfirm gcc cmake ninja python grpc protobuf` + SDL3's `libgl mesa libx11 libxcursor libxext libxfixes libxi libxinerama libxkbcommon libxrandr libxrender libxss libxtst wayland wayland-protocols`. Drops the PPA, `libcurl4-openssl-dev`, pip CMake. Format job unchanged. macOS support is removed (September decision; gRPC + GCC 16 on Homebrew is not worth carrying).

`justfile`: drop the macOS branch; `ci-build-test` uses `g++` when `g++-16` is absent (today `just ci` fails on Arch).

## 6. Docs

Rewrite `docs/architecture.md`, `docs/building.md` (gRPC/protobuf in, libcurl and macOS out, override vars), `docs/running.md` (new "Start llamad" section, `--socket`, `--play`, GUI Connection), `docs/testing.md`, `docs/world.md` (budget/round text with Pig Pen as owner), `docs/logs.md` (format v2), `README.md`, `CLAUDE.md` (deps, commands, invariants: "gRPC only in `pigpen_llamad`", "one worker thread per round inside the loop; tools and all callbacks on the pump thread", three seams). Scrub comments in `world.hpp`, `events.hpp`, `tool_responses.hpp`, `config.hpp`. Acceptance: `git grep -i scry` → nothing; also `base_url`, `PIGPEN_API_KEY`, `libcurl`, `OpenAI`.

## 7. Upstream issues (filed 2026-11-06)

Pig-pen-agnostic wording, as before.

**A. [llamad#42](https://github.com/crybo-rybo/llamad/issues/42) — let a Chat request turn reasoning off (`enable_thinking`)** — Qwen3/DeepSeek-style templates open a `<think>` block unless the template is rendered with `enable_thinking=false`. llama.cpp's `common_chat_templates_apply` already takes `chat_template_kwargs`; llamad exposes no way to set them, so an agent that wants terse tool calls gets reasoning text in `text` chunks and in the token bill. Proposal: `ChatRequest.reasoning` enum `{DEFAULT, OFF}` (or `map<string,string> chat_template_kwargs`), applied the same way the schema path already closes the think block. Additive.

**B. [llamad#43](https://github.com/crybo-rybo/llamad/issues/43) — `ModelInfo` should identify the model file and the daemon version** — `description` ("qwen2 0.5B Q4_K_M - Medium") is not unique across fine-tunes or quant files, and nothing on the wire says which llamad version answered. Logs that want provenance need `model_path` (basename, or the GGUF `general.name`) and `llamad_version`. Additive fields.

**C. [scrydantic#22](https://github.com/crybo-rybo/scrydantic/issues/22) — a tool lookup/dispatch helper** — every host writes the same loop over `std::vector<BoundTool>`: find by name, report an unknown tool, invoke. A `ToolSet` (or free `find_tool`) with a stable "unknown tool X; known tools: a, b, c" model message would remove the one piece of boilerplate the MCP example and pig-pen both hand-roll. Low priority.

Not filing: #18 (proto-only target) — `SOURCE_SUBDIR proto` already works; worth a comment on the issue saying so. #19/#21/#25 are real but pig-pen tolerates them.

## 8. Phasing (each PR green on its own)

1. **PR 1 — core seams (scry untouched):** `chat_client.hpp`, `tool_table.hpp`, `ToolTurnTransport`, `FakeChatClient`, `tool_turn_transport_tests.cpp`. Adds the scrydantic FetchContent and links only `scrydantic::json` into `pigpen_core`. `TurnOutcome` keeps its current fields (the loop fills `attempts` and `tool_stats`); the renames and the new token fields move to PR 3 with the log format. Because Scrydantic's configure step requires GCC 16.2, the CI container switch to Arch lands here too. ~700 lines + tests.
2. **PR 2 — llamad client:** gRPC/protobuf find_package, `llamad_proto` FetchContent, `pigpen_llamad`, `FakeLlamaService`, `llamad_chat_client_tests.cpp`, `pigpen_fake_llamad` executable. (The CI container switch already landed in PR 1.)
3. **PR 3 — the cut-over:** scrydantic dependency; `WorldToolbox`, dispatch adapter, `MetricsWriter` on scrydantic, `Session` rewired; **delete** `scry_transport.*`, `world_tool_binding.*`, `scry_transport_tests.cpp`, the scry FetchContent; `Config`/CLI/GUI flags; Python tests on the fake daemon; nlohmann removed. This is the one unavoidably large PR.
4. **PR 4 — docs and housekeeping:** every doc, `CLAUDE.md`, README; delete stale branches (`chore/scry-v0.7.0`, `chore/bump-scry-v0.3.0`, `origin/feature/scry-tool-observation`, `origin/claude/pig-pen-scry-v0.5-migration-*`); retire the scry memory note.

Alternative: one branch, one PR. Recommend the four above; 1 and 2 are independently reviewable and keep `main` shippable.

## 9. Risks and open consequences

- **The unmerged benchmark branch** (`origin/claude/benchmark-leaderboard`, `bench/pigpen_bench.py`) assumes one Ollama server serving many models (`--base-url`, `--model`, `GET /models`, `/api/show`), and reads `turn.scry_tools.calls`. After this refactor it must spawn one `llamad --model X.gguf --socket …` per model and read `turn.tools.requested`. Not in this plan's scope; flagging so it is a conscious follow-up.
- **llamad is pre-1.0**: a minor release may change the proto. Pin by tag; the generated code and the `LlamadChatClient` are the only places that notice.
- **No reasoning toggle** (issue A): until it lands, run instruct models or accept `<think>` text in transcripts; pig-pen will not strip it.
- **Cancel during prefill** (#25): the client returns promptly, but the daemon keeps decoding; a GUI Reset followed by Play queues behind the abandoned prefill. Cosmetic latency, no correctness issue.
- **Greedy non-determinism from the KV cache** (documented by llamad): temperature 0 + same seed can still differ between a warm and a cold daemon. The world stays deterministic; the log records it either way.
- **Log format break**: field renames are intentional; `docs/logs.md` is the contract. Old logs remain readable, just differently shaped.
- **Ubuntu CI is gone**; anyone building on Debian/Ubuntu needs a GCC 16.2 and gRPC with CMake config files (`docs/building.md` will say Arch is the tested platform; the GCC image route scrydantic's CI uses is a fallback if the Arch container ever misbehaves).

## 10. Decisions (taken 2026-11-06)

1. GUI auto-start is the explicit `--play` flag.
2. Four PRs as in §8, stacked (§12).
3. nlohmann/json is dropped; tests read JSON with `scrydantic::JsonView`.
4. Issues A, B, C are filed (links in §7).
5. `tool_error` records are included: the dispatcher already holds the text, so the record is one reflected struct and one `write_line`, and it is the only way protocol failures become observable.
6. Confirmed: macOS dropped; `--model` and `PIGPEN_API_KEY` removed; `ModelInfo.description` is the log's `model` until issue B lands.
7. The benchmark branch may break; it is a separate follow-up.

## 11. Verification (end state)

1. `just ci` (format, lint, dev/release/headless builds + tests) on Arch; CI green in the container.
2. `ctest --preset dev -R "turn_transport|llamad|headless"`; `./build/dev/pigpen_reflection_tests`.
3. Isolation: run `ctest` while a real daemon listens on the default socket; its stderr shows no request from the suite.
4. Manual e2e: `llamad --model ~/Work/llamad/models/qwen2.5-0.5b-instruct-q4_k_m.gguf --socket /tmp/pigpen-e2e.sock`, then `./build/dev/pig-pen-headless --socket /tmp/pigpen-e2e.sock --turns 2 --seed 42` → exit 0, header `model`/`context_tokens` set, `eat {}` accepted by the grammar, `cached_prompt_tokens > 0` on turn 2.
5. Exit codes: daemon down → 1 and no `logs/`; SIGINT mid-turn → 130 with footer; `--timeout-seconds 1` → 3; unwritable log dir → 4; text-only scenario → 5.
6. GUI: Reset with the daemon down reports within 2 s; Reset mid-turn returns promptly; Stats shows the new token fields.
7. `git grep -iE 'scry|base_url|PIGPEN_API_KEY|libcurl|OpenAI'` → empty.

## 12. Execution

- Branches, each based on the previous: `feat/llamad-1-core-seams` → `feat/llamad-2-grpc-client` → `feat/llamad-3-cutover` → `feat/llamad-4-docs`. PR *n* opens with base = branch *n-1* (PR 1 against `main`); after a merge, retarget the next PR to `main`.
- Each PR is implemented by an Opus 5.5 worker from a brief that quotes the relevant sections of this plan, then reviewed (build, `ctest`, diff read, `just fmt-check`, `git grep scry` for PR 3+) before the PR is opened.
- Commit messages follow the repo's `area: what changed` style; PR bodies list the plan section, verification run, and gaps.
