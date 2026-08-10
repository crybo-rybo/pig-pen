# World and tools

The world is a plain C++ value type (`pigpen::world::World`) with no knowledge
of models, JSON, or networking. Typed argument and response aggregates form the
model boundary; Scry derives their schemas and JSON marshalling with C++26
reflection.

## The grid

- 10×10, walled on all four sides. Coordinates run `0..9`.
- `(0,0)` is the **south-west** corner: `x` grows east, `y` grows north.
- The blob spawns at `(5,5)`. That cell is empty and starts out as the only
  observed cell.

## Items

| item | reward | count |
|---|---:|---:|
| berry | +1 | 6 |
| apple | +3 | 3 |
| truffle | +10 | 1 |
| toadstool | −5 | 3 |

Thirteen items are placed at construction from the seed. The truffle is drawn
first and constrained to a cell at least 4 Manhattan steps from the spawn, so
the single high-value item is never sitting next to the blob; the rest fill
random free cells. Nothing is placed on the spawn.

An episode's objective is complete when every **positive-value** item is gone.
Leftover toadstools do not keep it running.

## Determinism

The seed plus the action sequence fully determine the world. `World::dump()`
serialises seed, position, score, the full item grid, the observed bitset, and
per-item eaten counts into one canonical string — that is what the tests
compare, and it is why two runs with the same seed and the same tool calls are
byte-identical. The bounded random draw is implemented by hand rather than with
`std::uniform_int_distribution`, whose mapping is not portable across standard
libraries, so a seed means the same pen on every supported compiler.

## Tools

The model gets exactly three tools. Their JSON Schemas are compile-time
artifacts generated from the declarations in `tool_contract.hpp`; member names
become property names and the `world::Direction` enumerators become the accepted
strings. Generated object schemas set `additionalProperties: false`, so extra
arguments are rejected rather than ignored.

Pig Pen executes at most four world-tool actions per conversation turn. Every
successfully decoded call returns a flat reflected response with common status
and budget fields plus the tool-specific fields:

```json
{
  "action_executed": true,
  "error": null,
  "error_code": null,
  "ok": true,
  "...": "tool-specific fields",
  "turn_tool_budget": {
    "used": 3,
    "remaining": 1,
    "instruction": "1 world-tool call remains in this turn."
  }
}
```

The fourth result tells the model to return its final summary. Further calls
reach the typed handler and are logged, but do not change the world; they return
`ok: false`, `action_executed: false`, and a `tool_budget_exhausted` error.

### `look(direction)`

Scans every cell from the blob to the wall in one direction and marks them all
observed.

```json
{"direction": "north"}
```
```json
{
  "action_executed": true,
  "cells": [{"distance": 1, "item": null}, {"distance": 2, "item": "berry"}],
  "direction": "north",
  "error": null,
  "error_code": null,
  "ok": true,
  "turn_tool_budget": {"used": 1, "remaining": 3, "instruction": "3 world-tool calls remain in this turn."},
  "wall_at_distance": 5
}
```

`cells` never includes the blob's own cell; `distance` starts at 1.
`wall_at_distance` is one past the last visible cell.

### `move(direction)`

Steps exactly one cell and marks the destination observed. **Moving never eats
or collects anything** and never changes the score — that is the mechanic
models most often get wrong.

```json
{
  "action_executed": true,
  "error": null,
  "error_code": null,
  "item_here": "apple",
  "ok": true,
  "position": {"x": 5, "y": 6},
  "reason": null,
  "turn_tool_budget": {"used": 1, "remaining": 3, "instruction": "3 world-tool calls remain in this turn."}
}
```

Walking into a wall is a normal, recoverable outcome, not an error:

```json
{
  "action_executed": true,
  "error": null,
  "error_code": null,
  "item_here": null,
  "ok": false,
  "position": {"x": 5, "y": 9},
  "reason": "wall",
  "turn_tool_budget": {"used": 1, "remaining": 3, "instruction": "3 world-tool calls remain in this turn."}
}
```

### `eat()`

Takes **no arguments** — an empty object. Consumes the item on the current
cell and applies its reward to the score.

```json
{
  "action_executed": true,
  "ate": "truffle",
  "error": null,
  "error_code": null,
  "ok": true,
  "reason": null,
  "reward": 10,
  "score": 11,
  "turn_tool_budget": {"used": 1, "remaining": 3, "instruction": "3 world-tool calls remain in this turn."}
}
```
```json
{
  "action_executed": true,
  "ate": null,
  "error": null,
  "error_code": null,
  "ok": false,
  "reason": "nothing_here",
  "reward": null,
  "score": null,
  "turn_tool_budget": {"used": 1, "remaining": 3, "instruction": "3 world-tool calls remain in this turn."}
}
```

### Tool errors

Scry owns JSON parsing, schema validation, and reflected decoding. Calls with
unknown tools or invalid arguments are rejected at that boundary and never
enter `WorldToolController`, consume Pig Pen's action budget, or create a
tool activity.

A successfully decoded call beyond the per-turn action limit does reach
`WorldTools` and returns a flat typed failure without executing an action:

```json
{
  "action_executed": false,
  "error": "No action was executed because this turn's world-tool call budget is exhausted.",
  "error_code": "tool_budget_exhausted",
  "item_here": null,
  "ok": false,
  "position": {"x": 5, "y": 5},
  "reason": null,
  "turn_tool_budget": {
    "used": 4,
    "remaining": 0,
    "instruction": "Tool budget exhausted for this turn. Return your final summary now without calling another tool."
  }
}
```

Each successfully decoded handler invocation publishes exactly one typed
`ToolActivity` to the session's journal. The activity records its outcome —
`budget_exhausted` means no action executed — so budget rejections remain
observable without being animated as world actions.

## What the model is told

Three `Config` flags change the model's view without changing the simulation.
The world, the score, and the log always record the truth.

| flag | CLI | effect |
|---|---|---|
| `known_item_values` | `--hidden-values` turns it off | on: the system prompt lists the full reward table. Off: it says values are hidden and must be inferred from tool feedback. |
| `reward_feedback` | `--no-reward-feedback` turns it off | on: a successful `eat` returns numeric `reward` and `score`. Off: those fixed response fields are `null`. |
| `opaque_look` | `--opaque-look` turns it on | on: `look` reports an occupied cell as `"something"` instead of naming the item. |

The system prompt is assembled in `src/agent/prompt.cpp` and describes the
coordinate system, the three tools, the flags in force, and the turn and
tool-round budgets. Each turn is then advanced by a short generated nudge.
Human guidance is queued FIFO and delivered in its own labelled section, one
message per turn. If a completed turn produces no decoded world-tool call,
the next automatic nudge explicitly requires a tool call before more narration.
