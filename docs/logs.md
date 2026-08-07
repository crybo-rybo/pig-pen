# Logs

Every session — CLI or GUI — writes one JSONL file:

```
logs/<timestamp>-<model>-<seed>.jsonl
```

for example `logs/20260807-064512-538-qwen3_0.6b-42.jsonl`. The timestamp is
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
{"type":"header","model":"qwen3:0.6b","base_url":"http://127.0.0.1:11434/v1",
 "seed":42,"started_at":"2026-08-07T06:45:12-0400","prompt_variant":"default",
 "scenario":{"grid":{"width":10,"height":10},"spawn":{"x":5,"y":5},
   "items":{"berry":6,"apple":3,"truffle":1,"toadstool":3},
   "turn_budget":2,"max_tool_rounds":8,
   "known_item_values":true,"reward_feedback":true,"opaque_look":false}}
```

`prompt_variant` is whatever you passed to `--prompt-variant`, or the GUI
preset name. Everything needed to reproduce the run is in this line.

## `tool`

One line per attempted tool call, including calls rejected by schema
validation.

```json
{"type":"tool","turn":1,"tick":2,"tool":"look",
 "args":{"direction":"south"},
 "result":{"direction":"south","cells":[{"distance":1,"item":"berry"}],"wall_at_distance":6},
 "before":{"x":5,"y":5},"after":{"x":5,"y":5},"score_after":0}
```

`tick` is a monotonic counter across the whole episode. `before`/`after` are
the blob's position either side of the call — identical for `look`, `eat`, a
wall-blocked `move`, and any rejected call. `result` is exactly the JSON the
model received, so a log made with `--opaque-look` shows `"something"` here
too.

## `turn`

One line per conversation turn, flushed as it completes.

```json
{"type":"turn","turn":1,"status":"completed",
 "user_message":"Continue exploring autonomously. ... Turn 1 of 2.",
 "assistant_text":"I have explored the pen and found valuable items. ...",
 "error":"","input_tokens":1699,"output_tokens":198,"latency_ms":2499}
```

`status` is `completed`, `cancelled`, or `error`. Token counts come from the
provider; `latency_ms` is measured locally around the turn.

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

Header, footer, and every tool call at a glance:

```sh
jq -s '{header: first, footer: last, tools: [.[] | select(.type == "tool")]}' \
  logs/<run>.jsonl
```

Just the action trace:

```sh
jq -r 'select(.type=="tool")
  | "\(.tick) \(.tool) \(.args) -> \(.result | tostring[0:80])"' logs/<run>.jsonl
```

Calls the schema rejected — a good first stop when a model scores nothing:

```sh
jq -c 'select(.type=="tool" and .result.error_code)' logs/<run>.jsonl
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
