# Logs

Every session — CLI or GUI — writes one JSONL file:

```
logs/<timestamp>-<model>-<seed>.jsonl
```

for example `logs/20260807-064512-538-acme_pig-model_Q4_K_M-42.jsonl`. The timestamp is
local time down to the millisecond, and characters that are awkward in
filenames are replaced with `_`. If the name somehow collides, a `-1`, `-2`, …
suffix is appended; an existing log is never overwritten. Use `--log-dir` to
write somewhere other than `logs/`.

A file always has exactly one `header` line first and one `footer` line last,
with `tool` and `turn` records in between, in the order they happened. Object
keys are emitted in sorted order.

## `header`

Written when the session is created, before the model is contacted.

```json
{"type":"header","model":"acme/pig-model:Q4_K_M","base_url":"http://127.0.0.1:11434/v1",
 "temperature":0.2,"max_output_tokens":8096,"seed":42,"started_at":"2026-08-07T06:45:12-0400","prompt_variant":"default",
 "scenario":{"grid":{"width":10,"height":10},"spawn":{"x":5,"y":5},
   "items":{"berry":6,"apple":3,"truffle":1,"toadstool":3},
   "turn_budget":2,"max_tool_rounds":8,"max_world_tool_calls_per_turn":4,
   "known_item_values":true,"reward_feedback":true,"opaque_look":false}}
```

`prompt_variant` is whatever you passed to `--prompt-variant`, or the GUI
preset name. `temperature` records model sampling separately from the
deterministic world `seed`, and `max_output_tokens` records the per-request
limit Pig Pen asks the provider to apply. `max_world_tool_calls_per_turn`
records the hard action cap enforced by Pig Pen. Everything needed to describe
the run is in this line, although model sampling is not guaranteed to be
reproducible.

## `tool`

One line per successfully decoded reflected world-tool invocation, serialized
from the same typed `ToolActivity` record the UI, animation, and statistics
consume. Calls Scry rejects during protocol or schema validation do not enter
Pig Pen's application layer and therefore do not produce a `tool` record.

```json
{"type":"tool","turn":1,"tick":2,"tool":"look","outcome":"succeeded",
 "direction":"south","before":{"x":5,"y":5},"after":{"x":5,"y":5},
 "eaten":null,"action_executed":true,"score_after":0,
 "summary":"look south: 1 occupied of 5 cells, wall at distance 6"}
```

`tick` is a monotonic counter across the whole episode. `outcome` is one of
`succeeded`, `blocked_by_wall`, `nothing_to_eat`, or `budget_exhausted`;
`action_executed` is the derived convenience flag that is `false` only for
`budget_exhausted`, when Pig Pen's per-turn action budget rejected the call
before it could touch the world. `before`/`after` are the blob's position
either side of the call — identical for `look`, `eat`, a wall-blocked `move`,
and a budget rejection.

The record describes Pig Pen semantics rather than mirroring the provider
payload, and it always records the truth: `eaten`, `score_after`, and
`summary` come from the world, so a log made with `--opaque-look` or
`--no-reward-feedback` still shows real items and real scores even though the
model saw less.

## `turn`

One line per conversation turn, flushed as it completes.

```json
{"type":"turn","turn":1,"status":"completed",
 "user_message":"Automatic turn instructions:\nContinue exploring autonomously. ... Turn 1 of 2.",
 "assistant_text":"I have explored the pen and found valuable items. ...",
 "error":"","input_tokens":1699,"output_tokens":198,"tool_calls":2,
 "zero_tool_turn":false,"latency_ms":2499}
```

`status` is `completed`, `cancelled`, or `error`. `tool_calls` is counted from
successfully decoded reflected handler invocations for this turn and
`zero_tool_turn` makes narration-only or invalid-call-only turns easy to query.
Token counts come from the provider; `latency_ms` is measured locally around
the turn.

## `footer`

```json
{"type":"footer","complete":true,"finish_reason":"turn_budget","error":"",
 "final_score":0,"items_eaten":{"apple":0,"berry":0,"toadstool":0,"truffle":0},
 "tool_call_counts":{"eat":6,"look":3,"move":0},
 "turns_used":2,"duration_ms":3186}
```

`complete: true` means the episode reached a terminal state on its own —
`turn_budget`, `objective_complete`, `stopped`, `cancelled`, or `error`. If the
process exits, the window closes, or the session is reset mid-episode, the
writer still emits a footer, but with `complete: false` and
`finish_reason: "abandoned"`. A footer is final: nothing can be recorded after
it, and it cannot be written twice.

## Reading a log

Header, footer, and every decoded world-tool call at a glance:

```sh
jq -s '{header: first, footer: last, tools: [.[] | select(.type == "tool")]}' \
  logs/<run>.jsonl
```

Just the decoded-call trace (including budget rejections):

```sh
jq -r 'select(.type=="tool")
  | "\(.tick) [\(.outcome)] \(.summary)"' logs/<run>.jsonl
```

Calls rejected by Pig Pen's per-turn action budget:

```sh
jq -c 'select(.type=="tool" and (.action_executed | not))' logs/<run>.jsonl
```

Compare the outcome of several runs:

```sh
for f in logs/*.jsonl; do
  jq -c --arg f "$f" 'select(.type=="footer")
    | {run:$f, score:.final_score, reason:.finish_reason, tools:.tool_call_counts}' "$f"
done
```

Since the world is seed-deterministic, two runs with the same seed and turn
budget differ only in what the model did — the ordered `tool` records line up
directly.
