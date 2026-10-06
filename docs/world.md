# World and tools

The world is a plain C++ value type (`pigpen::world::World`). It has no data
about models, JSON, or networks. Typed argument aggregates and typed response
aggregates make the boundary to the model. Scry uses C++26 reflection to make
their schemas and their JSON conversion.

## The grid

- The grid is 10×10, with walls on all four sides. Coordinates are `0..9`.
- `(0,0)` is the **south-west** corner. `x` increases to the east, and `y`
  increases to the north.
- The blob starts at `(5,5)`, the spawn cell. The spawn cell is empty. At the
  start, it is the only observed cell.

## Items

| item | reward | count |
|---|---:|---:|
| berry | +1 | 6 |
| apple | +3 | 3 |
| truffle | +10 | 1 |
| toadstool | −5 | 3 |

When Pig Pen creates the world, it uses the seed to put 13 items in the grid.
It puts the truffle first, in a cell that is a minimum of 4 Manhattan steps
from the spawn cell. This makes sure that the item with the highest value is
never next to the blob at the start. Pig Pen puts the other items in random
empty cells. The spawn cell never has an item.

When no items with a positive value remain, the objective of the episode is
complete. Toadstools that remain do not keep the episode active.

## Determinism

The seed and the action sequence fully set the state of the world.
`World::dump()` serializes these values into one canonical string:

- the seed
- the position
- the score
- the full item grid
- the observed bitset
- the eaten count for each item

The tests compare these strings. Two runs with the same seed and the same tool
calls give strings that are identical, byte for byte. Pig Pen uses its own code
for the bounded random draw. It does not use `std::uniform_int_distribution`,
because that class maps values differently in different standard libraries. Because of this, a seed gives the same pen with all supported
compilers.

## Tools

The model gets exactly three tools. Scry makes their JSON Schemas at compile
time from the declarations in `tool_contract.hpp` and `tool_responses.hpp`:

- Member names become property names.
- The `world::Direction` enumerators become the accepted strings.
- Object schemas set `additionalProperties: false`. Scry rejects extra
  arguments. It does not ignore them.

A decoded world-tool call is a call that Scry admits and decodes. Each decoded
call returns a flat reflected world result. This result has an `ok` field and
the fields of the tool, as shown below. For the call budget and the round limit
of each turn, refer to [Tool errors and limits](#tool-errors-and-limits).

### `look(direction)`

`look` scans all cells from the blob to the wall in one direction. It marks
all these cells as observed.

Example arguments:

```json
{"direction": "north"}
```

Example result:

```json
{
  "cells": [{"distance": 1, "item": null}, {"distance": 2, "item": "berry"}],
  "direction": "north",
  "ok": true,
  "wall_at_distance": 5
}
```

`cells` never includes the cell of the blob. `distance` starts at 1.
`wall_at_distance` is the distance of the last visible cell plus one.

### `move(direction)`

`move` moves the blob exactly one cell, and marks the destination cell as
observed. **A move never eats or collects an item**, and it never changes the
score. Models make mistakes with this rule more frequently than with other
rules.

Example result:

```json
{
  "item_here": "apple",
  "ok": true,
  "position": {"x": 5, "y": 6},
  "reason": null
}
```

If the blob moves into a wall, the result is a normal outcome, and the model
can recover. It is not an error:

```json
{
  "item_here": null,
  "ok": false,
  "position": {"x": 5, "y": 9},
  "reason": "wall"
}
```

### `eat()`

`eat` takes **no arguments**. Send an empty object. `eat` eats the item in the
current cell and adds its reward to the score.

Example result when the cell has an item:

```json
{
  "ate": "truffle",
  "ok": true,
  "reason": null,
  "reward": 10,
  "score": 11
}
```

Example result when the cell is empty:

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

Scry parses the JSON, validates the schema, decodes the reflected arguments,
and controls the call budget. The call budget is a maximum of four tool
requests in each conversation turn, across all rounds and batches. Requests for
unknown tools and requests with arguments that are not valid also use the
budget.

These requests and over-budget requests never go into `WorldTools`, and they
do not make world activity. For an over-budget call, Scry sends a tool error
that tells the model to give a summary. Each error gives the model sufficient
data to correct the request. Examples:

```json
{"error":"$.direction is not a declared enumerator; must be one of: north, south, east, west"}
```

```json
{"error":"tool call limit for this turn reached; respond without calling tools"}
```

The Scry admission hook also refuses all subsequent actions after one of these
events:

- The objective is complete.
- Pig Pen cannot write the episode log.

This includes subsequent calls in the same batch. The current turn can finish,
and it keeps the results of the calls that ran. Typical world outcomes, for
example a wall or an empty cell, are typed results. They are not protocol
errors.

Pig Pen uses `ToolRoundLimitPolicy::complete`. When a turn gets to
`max_tool_rounds`, Scry commits the rounds that ran and keeps their history.
Calls after that round limit do not run. The turn record in the log shows the
number of these calls. The next automatic turn instruction tells the model that
these calls did not run.

## What the model sees

Three `Config` flags change the data that the model sees. They do not change
the simulation. The world, the score, and the log always record the true
values.

| flag | CLI | effect |
|---|---|---|
| `known_item_values` | `--hidden-values` sets it to off | On: the system prompt shows the full reward table. Off: the system prompt says that the values are hidden, and that the model can find them only from the tool feedback. |
| `reward_feedback` | `--no-reward-feedback` sets it to off | On: a successful `eat` returns the numeric `reward` and `score`. Off: these fields stay in the response, but their values are `null`. |
| `opaque_look` | `--opaque-look` sets it to on | On: `look` shows an occupied cell as `"something"`, and does not name the item. |

`src/core/prompt.cpp` creates the system prompt. The system prompt describes
these items:

- the coordinate system
- the three tools
- the flags that are set
- the turn budget and the tool-round budget

For each turn, Pig Pen then sends a short automatic turn instruction. Pig Pen
keeps human guidance in a FIFO queue. It sends one guidance message for each
turn, in a section with its own label. If a completed turn has no decoded
world-tool call, the next automatic turn instruction tells the model to call a
tool first. Then the model can write more narration.
