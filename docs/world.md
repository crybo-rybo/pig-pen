# World and tools

The world is a plain C++ value type (`pigpen::world::World`) with no knowledge
of models, JSON, or networking. `ToolExecutor` wraps it with the exact schemas
the model sees.

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

The model gets exactly three tools. Schemas set `additionalProperties: false`,
so extra arguments are a hard validation error rather than being ignored.

### `look(direction)`

Scans every cell from the blob to the wall in one direction and marks them all
observed.

```json
{"direction": "north"}
```
```json
{
  "direction": "north",
  "cells": [{"distance": 1, "item": null}, {"distance": 2, "item": "berry"}],
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
{"ok": true, "position": {"x": 5, "y": 6}, "item_here": "apple"}
```

Walking into a wall is a normal, recoverable outcome, not an error:

```json
{"ok": false, "reason": "wall", "position": {"x": 5, "y": 9}}
```

### `eat()`

Takes **no arguments** — an empty object. Consumes the item on the current
cell and applies its reward to the score.

```json
{"ok": true, "ate": "truffle", "reward": 10, "score": 11}
```
```json
{"ok": false, "reason": "nothing_here"}
```

### Tool errors

Malformed calls come back as a structured result with an `error_code` of
`unknown_tool`, `malformed_json`, or `invalid_arguments`:

```json
{"ok": false, "error": "eat arguments must be an empty object",
 "error_code": "invalid_arguments"}
```

Every invocation appends exactly one event to the feed — including rejected
ones, which record identical before/after positions. That is deliberate: a
model that keeps sending `eat({"object": "apple"})` is visible in the event log
and in the JSONL, instead of silently disappearing.

## What the model is told

Three `Config` flags change the model's view without changing the simulation.
The world, the score, and the log always record the truth.

| flag | CLI | effect |
|---|---|---|
| `known_item_values` | `--hidden-values` turns it off | on: the system prompt lists the full reward table. Off: it says values are hidden and must be inferred from tool feedback. |
| `reward_feedback` | `--no-reward-feedback` turns it off | on: a successful `eat` returns `reward` and `score`. Off: it returns only `ate`. |
| `opaque_look` | `--opaque-look` turns it on | on: `look` reports an occupied cell as `"something"` instead of naming the item. |

The system prompt is assembled in `src/agent/prompt.cpp` and describes the
coordinate system, the three tools, the flags in force, and the turn and
tool-round budgets. Each turn is then advanced by a short generated nudge, with
any queued human guidance appended to it.
