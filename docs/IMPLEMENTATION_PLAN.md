# pig-pen implementation plan

This plan turns [`DESIGN.md`](DESIGN.md) into independently verifiable,
test-driven increments. The headless core is the product boundary; the GUI is
an adapter over the same runtime and never owns simulation rules.

## Decisions

- Use C++23, CMake 3.25+, Ninja presets, Catch2, nlohmann/json, scry, Dear
  ImGui docking, GLFW, and OpenGL 3. Pin every fetched dependency. A
  `PIGPEN_SCRY_SOURCE` override supports development against a sibling scry
  checkout while fresh clones fetch the pinned revision.
- Keep the specified 10x10 world. The deterministic blob spawn is `(5,5)`;
  its cell starts observed. Default placement excludes the spawn cell and
  puts the truffle at Manhattan distance at least four from it.
- Keep world code free of JSON, scry, GUI, filesystem, clock, and networking.
  It exposes typed results which the agent boundary serializes.
- Put JSON validation and result shaping in `ToolExecutor`. Invalid JSON or
  schema violations become tool errors; legal actions that fail a world rule
  remain successful structured results.
- Drive episodes through an `ITurnTransport` interface. Production adapts
  scry; tests use a fake transport and complete turns synchronously under
  explicit test control.
- Recreate the harness, conversation, tool executor, world, and runner as one
  session on Reset. This respects scry's additive-only tool registry and makes
  queued callbacks harmless through weak session state.
- Default to Ollama's OpenAI-compatible endpoint
  `http://127.0.0.1:11434/v1`, model `qwen3:8b`, 20 turns, 8 tool rounds per
  turn, deterministic temperature, reasoning disabled, and bounded output.
  Live probes showed provider-default Qwen reasoning consuming the entire
  response bound without completing; the explicit mode produces finite,
  action-focused turns. Headless CLI flags can reduce the turn budget and add
  an overall timeout for smoke tests.
- A user message entered in the GUI is queued for the next episode turn. The
  system prompt and automatic `Continue. Turn N of M.` nudges are sufficient
  to start and sustain exploration without user input.
- JSONL is append-only with header, tool, turn, and footer records. A log is
  finalized for every terminal path, including stop and model error. Destruction
  before a terminal callback writes an explicit incomplete `abandoned` footer.

## Test-first sequence

### 1. Build skeleton

Write the build graph and a trivial test first. Verify configure, compile, and
CTest under the `dev` preset before adding behavior. Keep GUI build optional
for genuinely headless environments, but enabled by default.

Acceptance:

- `cmake --preset dev`, `cmake --build --preset dev`, and
  `ctest --preset dev` succeed with warnings as errors.
- Both `pig-pen-headless` and `pig-pen` are build targets.

### 2. Pure world

Write failing tests for bounds, compass coordinates, observation rays,
fog-of-war state, item counts and values, truffle distance, uniqueness,
eating, score changes, positive-item exhaustion, and seeded dumps. Implement
only enough typed world behavior to pass them.

Acceptance:

- Every rule in DESIGN section 4 has a focused unit test.
- Two worlds with the same seed have byte-identical dumps after the same
  actions.

### 3. Agent boundary and tools

Write tests against `ToolExecutor` before registering it with scry: exact
schemas, malformed and extra arguments, direction parsing, wall/empty-cell
rule failures, successful result shapes, reward-feedback hiding, opaque look,
and one event per valid invocation. Add prompt snapshot assertions for every
experiment toggle.

Acceptance:

- `move`, `look`, and `eat` results match DESIGN section 5.
- World events contain turn, monotonic tick, arguments, result, and before/after
  positions needed by the GUI.

### 4. Episode state machine and metrics

Use a fake `ITurnTransport` to test idle/play/pause/resume/stop/finished,
single in-flight turn, automatic nudges, queued human input, turn/error/cancel
completion, item-exhaustion termination, and budget termination. Test JSONL by
parsing every emitted line and reconciling footer counts with tool records.

Acceptance:

- No execution path can exceed the configured turn budget.
- A terminal error is visible and is never silently retried by the episode
  layer.
- All terminal paths close a complete metrics log.

### 5. Local-model headless runner

Adapt `ITurnTransport` to scry, register tools once, and pump `Harness::update`
inside a small console loop. Add CLI/config validation, SIGINT/timeout stop,
live transcript/tool output, and a nonzero exit when the episode has no tool
calls.

Acceptance:

- A bounded run against local `qwen3:8b` finishes and produces parseable JSONL.
- The recorded header, tool records, turns used, score, counts, and footer
  reconcile.

### 6. Minimal GUI

Build dockable World, Transcript, Event Log, Controls, and Stats panels over a
shared session. Render a human-omniscient grid plus observed-cell overlay,
queue discrete move animations, and flash look/eat actions. Wire
Play/Pause/Stop/Reset, runtime endpoint/model/seed/scenario settings, and a
next-turn user input box.

Acceptance:

- The GUI and headless binary use the same world, tools, runner, prompt, and
  metrics code.
- Reset applies a new seed/config without process restart.
- A burst of move events is displayed sequentially rather than teleporting.

### 7. Experiment presets and completion audit

Expose known/hidden values, reward feedback, and opaque look as config toggles
and named presets. Document repeatable same-seed commands and a `jq` comparison
workflow. Finally audit each DESIGN requirement against tests, source, build
output, one bounded Ollama run, its log, and a GUI startup smoke check.

Acceptance:

- Presets are data, not code forks, and are serialized in every log header.
- The test suite, headless Ollama smoke, JSONL validator, and GUI startup smoke
  all pass.

## Completion audit — 2026-08-06

- A clean `release` configure fetched every pinned dependency from its remote
  source, then built the GUI, headless runner, and tests with GCC 16 and
  warnings as errors. The independent Clang 22 build passed under the same
  policy.
- All 44 tests passed in GCC debug, GCC release, and Clang debug builds. The
  suite includes deterministic world/tool/runner/metrics/animation coverage
  plus subprocess SIGINT and SIGTERM cancellation tests that assert complete
  JSONL footers.
- The final `qwen3:8b`, seed-42, two-turn Ollama smoke completed at the turn
  budget with six tool calls (`look` 4, `move` 1, `eat` 1), score 1, and a
  reconciled footer. It required no user input.
- The four-run, two-model matrix for experiment axes 1 and 2 completed with
  truthful footers; its results and limitations are recorded in the README.
- The release GUI created a mapped GLFW window, pumped a live session, handled
  the window manager's close request, and exited successfully. The
  time-injected animation tests verify ordered burst playback independently of
  frame rate.
