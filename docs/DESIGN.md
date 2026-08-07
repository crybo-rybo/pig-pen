# pig-pen — Design Document

A C++ GUI application that visualizes a locally hosted LLM living inside a
small, observable world. The model is embodied as a blob in a 10×10 pen. It
perceives and acts on the world exclusively through tools registered with
[scry](https://github.com/crybo-rybo/scry); the GUI shows the human what the
model sees, decides, and does — in real time.

Status: implemented baseline. This document remains the source of truth; the
test-driven delivery sequence and resolved implementation choices are recorded
in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).

---

## 1. Goals

- **Visualize agency.** Show a human, live, how an LLM interacts with an
  environment: what it observed, which tool it called, what changed.
- **Exercise scry as a real integrator.** pig-pen is a consumer of scry's
  stable C++23 surface (`Config`, `Conversation`, `ToolRegistry`, `Turn`,
  `Harness`) inside a real GUI main loop — exactly the app shape scry is built
  for. Friction found here is upstream feedback for scry.
- **Enable reward experiments.** The world supports items with values, episode
  scoring, and metrics logging so we can run repeatable experiments (e.g. "does
  the model learn item values from feedback within one conversation?").
- **Stay local-first.** Default target is an OpenAI-compatible local server
  (Ollama at `http://127.0.0.1:11434/v1`). Anthropic's API works through the
  same scry `Config` but is not required for anything.

### Non-goals (v1)

- No training or fine-tuning — "reward" is in-context feedback and offline
  metrics only.
- No multi-agent worlds, no continuous physics, no pathfinding done *for* the
  model. The model does the thinking; the world is deliberately dumb.
- No networked or persistent world state beyond conversation/episode logs.
- No custom GUI toolkit — Dear ImGui is sufficient and matches scry's showcase.

---

## 2. Background: what scry gives us

Read `scry/docs/design/public-api.md` and `scry/examples/` before writing code.
The essentials that shape this design:

- `Harness::send()` starts one agentic turn (request → streamed output → tool
  dispatch → automatic resend of tool results → final answer) and never blocks.
- **Every callback and every tool handler runs synchronously inside
  `Harness::update()`, on the app thread.** This is the load-bearing property:
  world mutation and GUI animation triggers all happen on the render thread, so
  the world needs no locks.
- `ToolRegistry` is additive-only; register everything once at startup and
  discard the harness on registration failure.
- A failed or cancelled turn does **not** roll back tool side effects that
  already ran. World state and conversation state can diverge on error; the
  episode layer (§6) owns reconciliation (in v1: end the episode).
- `max_tool_rounds` bounds a single turn. Long episodes are therefore *multiple*
  `send()` calls, not one giant turn.
- Two direct precedents live in `scry/examples/`:
  - `npc/` — a 5×5 grid world driven by `look`/`move_*` tools, including the
    `ToolExecutionObserver` pattern we reuse for GUI animation hooks.
  - `imgui/chat_panel.*` — one async scry turn inside a Dear ImGui panel,
    including the weak-shared-state pattern for callback lifetime safety.

---

## 3. Architecture

Strict layering; lower layers never include higher ones.

```
┌────────────────────────────────────────────────────────┐
│ app/        GLFW window, GL context, ImGui frame,      │
│             main loop: poll → harness.update() → draw  │
├────────────────────────────────────────────────────────┤
│ ui/         Panels: WorldView, Transcript, EventLog,   │
│             Controls, Stats. Read world + event feed;  │
│             never mutate the world directly.           │
├────────────────────────────────────────────────────────┤
│ agent/      scry integration: tool registration,       │
│             prompts, episode loop, metrics writer      │
├────────────────────────────────────────────────────────┤
│ world/      Pure simulation. No scry, no ImGui, no I/O.│
│             Deterministic given (seed, action stream). │
└────────────────────────────────────────────────────────┘
```

- **`world/` is a plain C++ library with zero dependencies.** It must be fully
  testable headless. Every rule in §4 gets a unit test here.
- **`agent/` depends on `world/` and scry only.** It is also headless-testable:
  Milestone 1 drives a whole episode from a console binary before any GUI
  exists.
- **`ui/` + `app/`** depend on everything. GUI reads world state each frame
  (single-threaded, so direct reads are safe) and consumes an **event feed** —
  an append-only `std::vector<WorldEvent>` the agent layer pushes into from
  tool handlers and turn callbacks — for the transcript, log, and animations.

### Main loop (the whole app, in one place)

```cpp
while (!glfwWindowShouldClose(window)) {
  glfwPollEvents();
  harness.update({.time_budget = 2ms, .max_callbacks = 32}); // tools + callbacks fire here
  episode.tick();          // auto-advance: send next turn if idle and episode not done
  begin_imgui_frame();
  ui.draw(world, events, episode);
  render_imgui_frame();
}
```

---

## 4. World specification

### Grid

- 10×10 cells. Origin `(0,0)` is the **south-west** corner; `x` grows east,
  `y` grows north (matches the npc showcase's compass tools).
- The pen is walled: moving off the edge fails, it does not wrap.
- One blob (the agent). At most one item per cell. The blob's cell may contain
  an item (that's what `eat` consumes).

### Items

| type      | reward | notes                                   |
|-----------|-------:|-----------------------------------------|
| `berry`   |     +1 | common                                   |
| `apple`   |     +3 | uncommon                                 |
| `truffle` |    +10 | rare, at least 4 cells from spawn        |
| `toadstool` |   −5 | poison — eating it is a mistake          |

Counts per episode (default scenario): 6 berries, 3 apples, 1 truffle,
3 toadstools. Placement is drawn from a **seeded PRNG** (`std::mt19937_64`);
the same seed always produces the same world. The seed is visible and settable
in the GUI and recorded in every metrics log. The v1 blob spawn is `(5,5)`;
item placement excludes that initial cell.

### Determinism

`World` is deterministic given `(seed, sequence of actions)`. No wall-clock,
no unseeded randomness inside `world/`. This makes episodes replayable and
experiments comparable across models.

### Observability — the important rule

**The model never sees the full grid.** It knows the grid is 10×10 and where it
currently stands; everything else must be discovered through `look`. Partial
observability is what makes the behavior worth watching and the experiments
meaningful. The *human* sees everything in the GUI, including an overlay of
which cells the model has actually observed (fog-of-war styling on
never-observed cells).

---

## 5. Tool specification

Three tools, explicit JSON schemas, registered once at startup via
`harness.tools().add(...)`. All handlers validate their arguments at the
boundary and return structured JSON results; rule violations (walking into a
wall, eating on an empty cell) are **successful tool results describing the
failure**, not `scry::Error` — the model should be told what happened and get
to try again. Reserve `scry::Error` for malformed arguments.

Every handler also appends a `WorldEvent` (for the GUI) and a metrics record
(§7) before returning.

### `move`

Moves one cell in a compass direction.

```json
{
  "type": "object",
  "properties": {
    "direction": { "type": "string", "enum": ["north", "south", "east", "west"] }
  },
  "required": ["direction"],
  "additionalProperties": false
}
```

Result — success:
`{"ok": true, "position": {"x": 4, "y": 7}, "item_here": "berry" | null}`
Result — blocked:
`{"ok": false, "reason": "wall", "position": {"x": 4, "y": 9}}`

> Fallback: the scry npc showcase uses four zero-argument tools
> (`move_north`, …) because some small local models handle argument-free tools
> more reliably. Implement `move(direction)` first; if the chosen default model
> fumbles the enum argument in practice, add the four-tool variant behind a
> config flag rather than redesigning.

### `look`

Scans in a straight line from the blob's cell toward a direction, reporting
every cell up to and including the wall.

```json
{
  "type": "object",
  "properties": {
    "direction": { "type": "string", "enum": ["north", "south", "east", "west"] }
  },
  "required": ["direction"],
  "additionalProperties": false
}
```

Result:

```json
{
  "direction": "east",
  "cells": [
    {"distance": 1, "item": null},
    {"distance": 2, "item": "apple"},
    {"distance": 3, "item": null}
  ],
  "wall_at_distance": 4
}
```

Whether `look` reports the item *type* or just `"something"` is an experiment
axis (§7); default v1 behavior reports the type. Marks scanned cells as
observed for the GUI fog-of-war.

### `eat`

Consumes the item on the current cell, if any.

```json
{ "type": "object", "properties": {}, "additionalProperties": false }
```

Result — success: `{"ok": true, "ate": "apple", "reward": 3, "score": 7}`
Result — empty cell: `{"ok": false, "reason": "nothing_here"}`

Whether the numeric `reward`/`score` fields are included in the result is the
core knob for reward experiments (§7). Default v1: included.

---

## 6. Agent & episode loop

### Conversation shape

- One `scry::Conversation` per episode. System prompt states the embodiment
  contract: you are a blob in a walled 10×10 pen, coordinates convention, the
  three tools, and the objective for the current scenario (e.g. "maximize your
  score; eating costs nothing but toadstools are poisonous").
- An **episode** = N turns (default 20, configurable). Each turn is one
  `harness.send(conversation, objective_nudge, callbacks)`, where the nudge is
  a short fixed user message ("Continue. Turn 7 of 20."). `EpisodeRunner::tick()`
  fires the next `send()` when the previous turn finished and the run state is
  `playing`.
- Per-turn `max_tool_rounds` default: 8.
- Episode ends when: turn budget exhausted, all positive-value items eaten, the
  user presses Stop, or a turn ends in a terminal error (surface the error in
  the GUI; do not silently retry).

### Run states

`idle → playing ⇄ paused → finished`. Pause takes effect between turns (an
in-flight turn runs to completion; `Turn::cancel()` is wired to the Stop
button, and per scry's contract a cancelled turn's already-executed moves
stand). Reset tears down conversation + world and reseeds.

### Callback wiring

- `on_text_delta` → append to transcript panel (streamed thinking-out-loud).
- Tool handlers themselves → push `WorldEvent{tool, args, result, tick}` for
  the event log and to enqueue the blob's move animation.
- `on_finished` → mark turn complete, record turn metrics, let `tick()` decide
  whether to send the next turn.

Follow the imgui showcase's lifetime pattern: callbacks capture weak/shared
state so a torn-down panel or reset episode makes queued callbacks harmless.
Harness and conversation outlive all panels.

---

## 7. Reward experiments & metrics

### Metrics log

Every episode appends JSONL to `logs/<timestamp>-<model>-<seed>.jsonl`:

- one header record: model, base_url, seed, scenario config, prompt variant;
- one record per tool call: turn, tool, args, result, score after;
- one footer record: final score, items eaten by type, tool-call counts,
  turns used, wall-clock duration.

Plain JSONL, no database. Comparing runs is `jq` territory.

### Experiment axes (each a toggle in the scenario config, not a code fork)

1. **Known vs. hidden values.** Item table in the system prompt vs. "some food
   is better than others; find out."
2. **Reward feedback on/off.** `eat` returns `reward`/`score` vs. only
   `{"ok": true, "ate": "..."}` — does in-context learning of values survive
   losing the explicit signal?
3. **Opaque look.** `look` reports `"something"` instead of item types —
   forces exploratory eating.
4. **Model comparison.** Same seed + scenario across e.g. `qwen3:8b`,
   `llama3.1:8b`, a Claude model. Deterministic worlds make this a fair fight.

Scenario config is a single struct serialized into the header record; a small
preset dropdown in the GUI selects between saved scenarios.

---

## 8. GUI specification (Dear ImGui)

Single window, dockable panels:

- **WorldView** — the pen. Grid rendered with ImGui draw lists (no textures
  needed for v1): cell borders, fog-of-war tint on never-observed cells, item
  glyphs/colored shapes, and the blob. Blob movement animates (~150 ms lerp
  between cells) driven by the `WorldEvent` queue, so a burst of tool calls in
  one `update()` plays back as discrete visible steps rather than teleporting.
  A "look" flashes a directional cone/ray highlight; an "eat" pulses the cell.
- **Transcript** — the conversation: user nudges, streamed model text, and
  inline compact tool call/result rows. Start from scry's `chat_panel` example.
- **Event log** — flat, filterable list of every tool call with args and
  results. This is the debugging ground truth.
- **Controls** — base URL, model name, scenario preset, seed (with reroll),
  turn budget, experiment toggles (§7), Play/Pause/Stop/Reset, animation speed.
- **Stats** — current score, items eaten by type, turns used / budget,
  tool-call counts, last-turn latency.

Backend: GLFW + OpenGL 3. scry's imgui showcase deliberately brings only ImGui
core; pig-pen owns the backend choice.

---

## 9. Tech stack & repo layout

- **Language/toolchain:** C++23 (match scry; do *not* require the C++26
  reflection component — explicit schemas only). Warnings-as-errors on GCC and
  Clang.
- **Build:** CMake ≥ 3.25 with `CMakePresets.json` (`dev`, `release`), plus a
  `justfile` (`just build`, `just test`, `just run`) mirroring scry's workflow.
- **Dependencies via FetchContent, pinned by tag/commit:** scry, Dear ImGui
  (docking branch), GLFW. JSON handling inside pig-pen: nlohmann/json
  (scry's `scry::Json` is a text carrier; we parse/serialize at the tool
  boundary).
- **Tests:** Catch2 (or doctest — implementer's choice, pick one) covering
  `world/` rules, tool handler argument validation and result shapes, and
  episode-runner state transitions with a fake harness boundary.

```
pig-pen/
├── CMakeLists.txt / CMakePresets.json / justfile
├── DESIGN.md                # this file
├── src/
│   ├── world/               # grid, items, rules, seeded spawn (no deps)
│   ├── agent/               # scry glue: tools, prompts, EpisodeRunner, metrics
│   ├── ui/                  # panels
│   └── app/                 # main.cpp, window/GL/ImGui bootstrap
├── tests/
└── logs/                    # gitignored JSONL episode logs
```

---

## 10. Milestones

Each milestone is independently verifiable; don't start the next until the
acceptance check passes.

**M0 — Skeleton.** CMake + presets + justfile; fetches scry/ImGui/GLFW; opens
an empty ImGui window; CI-able `just build && just test` with one trivial test.
*Accept: clean build on GCC and Clang from a fresh clone.*

**M1 — World, headless.** `world/` complete per §4 with unit tests; seeded
determinism test (same seed ⇒ identical spawn); all rules covered.
*Accept: `just test` green; a seed printed twice gives identical world dumps.*

**M2 — Agent, headless.** Tools registered per §5; `EpisodeRunner` per §6;
metrics JSONL per §7; a console binary (`pig-pen-headless`) runs a full
episode against local Ollama and exits nonzero if the model never calls a
tool (mirror the npc showcase's validation stance).
*Accept: `just run-headless` against Ollama produces a complete JSONL log of a
finished episode.*

**M3 — GUI.** WorldView with fog-of-war + animations, Transcript, Event log,
Controls, Stats; Play/Pause/Stop/Reset wired.
*Accept: watch a full episode live; reset + reseed works without restart; no
frame hitches during tool bursts (animation queue drains smoothly).*

**M4 — Experiments.** Scenario presets + all §7 toggles; model/base-url
switching at runtime; comparison of two runs by seed documented in README with
a real example.
*Accept: run axes 1 and 2 on the same seed with two models, four JSONL logs,
and a short written comparison.*

---

## 11. Open questions (decide during M2–M4, don't block on them)

- Should the blob have an energy/step budget (moves cost, food restores)?
  Deferred: adds pressure that makes experiments richer, but complicates the
  baseline. Design the `World` so a step cost is a scenario parameter, not a
  rewrite.
- Turn nudge wording: does "Turn 7 of 20" measurably change behavior vs. a
  bare "Continue."? Worth an experiment axis later.
- Episode replay from JSONL logs in the GUI (scrubber over a finished run).
  Cheap to add later because logs already carry every event; keep the
  `WorldEvent` type serializable.
- Reasoning display: resolved for the local baseline. Live `qwen3:8b` probes
  exhausted bounded replies on hidden reasoning before completing turns, so v1
  requests `ReasoningMode::disabled` and displays streamed visible output.
  This keeps the run finite and the transcript action-focused.
