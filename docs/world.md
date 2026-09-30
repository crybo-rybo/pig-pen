# World and tools

The world is a plain C++ value type (`pigpen::world::World`) with no knowledge
of models, JSON, or networking. Typed argument and response aggregates form the
model boundary; Scry derives their schemas and JSON marshalling with C++26
reflection.

## The grid

- 10×10, walled on all four sides. Coordinates run `0..9`.
- `(0,0)` is the **south-west** corner: `x` grows east, `y` grows north.
- The blob spawns at `(5,5)`. That cell is empty and starts out as the only
  observed cell. Standing on or looking at a cell marks it observed for good;
  `World::observed_count()` is the number of distinct observed cells, spawn
  included (1 to 100).

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

## Reward

The world score above is the truth, and nothing below changes it. Separately,
every episode gets a shaped **reward** (version 1), computed after the fact
from what was recorded. It is meant for comparing and training policies. It
never reaches the world or the prompt. The model is not told about it.

Each term is a count times a weight:

| term (weight name) | default | counts |
|---|---:|---|
| `score` | 1.0 | points of truthful world score (may be negative) |
| `explored_cell` | 0.05 | distinct cells observed beyond the spawn (`observed_count − 1`, at most 99) |
| `active_turn` | 0.1 | completed turns with at least one executed world action |
| `zero_tool_turn` | −1.0 | completed turns with no executed world action |
| `failed_action` | −0.1 | executed actions that failed: a wall bump or an empty `eat` |
| `invalid_call` | −0.5 | requests naming an unknown tool or carrying undecodable arguments |
| `budget_refused_call` | −0.25 | requests past the four-request limit |
| `objective` | 5.0 | once, when the objective is complete |
| `unused_turn` | 0.1 | turns left in the budget, only when the objective is complete |

Cancelled or errored turns count as neither active nor zero-tool. Invalid and
over-budget requests come from each turn's call tally (see
[Logs](logs.md#turn)), and a turn without one adds nothing. Requests the host
refuses after the objective is complete are recorded but not penalised; they
only happen when the model batched more calls into the round of the final
`eat`.

A reward is **valid** only when the episode ended with `turn_budget` or
`objective_complete`. For `stopped`, `cancelled`, `error`, or an unfinished
episode, the reward is absent rather than zero: logs write `"total": null`
with an `invalid_reason`, and a trainer should drop the episode. The counts and
per-term contributions are still written, so any episode can be re-weighted
offline. Override weights with `--reward NAME=VALUE` on the CLI
([Running](running.md#options)) or the worker ([Training](training.md)); the
weights used are recorded in the log footer and in every worker record. The
worker also marks an episode cut short by its `--timeout-seconds` invalid,
with reason `timeout`.

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

Admitted, decoded calls return flat reflected world results with `ok` and the
tool-specific fields below. The per-turn call budget and round limit are
described under [Tool errors and limits](#tool-errors-and-limits).

### `look(direction)`

Scans every cell from the blob to the wall in one direction and marks them all
observed.

```json
{"direction": "north"}
```
```json
{
  "cells": [{"distance": 1, "item": null}, {"distance": 2, "item": "berry"}],
  "direction": "north",
  "ok": true,
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
  "item_here": "apple",
  "ok": true,
  "position": {"x": 5, "y": 6},
  "reason": null
}
```

Walking into a wall is a normal, recoverable outcome, not an error:

```json
{
  "item_here": null,
  "ok": false,
  "position": {"x": 5, "y": 9},
  "reason": "wall"
}
```

### `eat()`

Takes **no arguments** — an empty object. Consumes the item on the current
cell and applies its reward to the score.

```json
{
  "ate": "truffle",
  "ok": true,
  "reason": null,
  "reward": 10,
  "score": 11
}
```
```json
{
  "ate": null,
  "ok": false,
  "reason": "nothing_here",
  "reward": null,
  "score": null
}
```

### Tool errors and limits

Scry owns JSON parsing, schema validation, reflected decoding, and the call
budget: at most four tool requests per conversation turn across all rounds and
batches, with unknown tools and invalid arguments also spending it. Unknown
tools, invalid arguments, and over-budget requests never enter `WorldTools` or
create world activity; over-budget calls get a tool error asking the model to
summarize. The model receives an error with enough
information to correct the request, for example:

```json
{"error":"$.direction is not a declared enumerator; must be one of: north, south, east, west"}
```

```json
{"error":"tool call limit for this turn reached; respond without calling tools"}
```

Scry's admission hook also refuses subsequent actions when the objective is
complete or the episode log has failed, including later calls in the same batch.
The current turn can finish with its executed results intact. Ordinary world
outcomes such as a wall or an empty cell stay typed results, not protocol errors.

Pig Pen selects `ToolRoundLimitPolicy::complete`: reaching `max_tool_rounds`
commits the rounds already executed instead of discarding their history. Calls
requested beyond that round limit never run. Their count appears in the turn
log and the next automatic prompt tells the model they were not executed.

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
message per turn. If a completed turn produces no decoded world-tool event,
the next automatic nudge explicitly requires a tool call before more narration.
