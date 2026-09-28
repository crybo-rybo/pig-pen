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
keys are written in sorted order; the examples below group them for
readability instead.

## `header`

Written when the session is created, before the model is contacted.

```json
{"type":"header","model":"acme/pig-model:Q4_K_M","base_url":"http://127.0.0.1:11434/v1",
 "temperature":0.2,"max_output_tokens":8192,"seed":42,"sampling_seed":null,"started_at":"2026-08-07T06:45:12-0400","prompt_variant":"default",
 "scenario":{"grid":{"width":10,"height":10},"spawn":{"x":5,"y":5},
   "items":{"berry":6,"apple":3,"truffle":1,"toadstool":3},
   "turn_budget":2,"max_tool_rounds":8,"max_world_tool_calls_per_turn":4,
   "known_item_values":true,"reward_feedback":true,"opaque_look":false}}
```

`prompt_variant` is whatever you passed to `--prompt-variant`, or the GUI
preset name. `temperature` records model sampling separately from the
deterministic world `seed`. `sampling_seed` is the provider sampling seed from
`--sampling-seed` or the GUI, or `null` when none was sent. `max_output_tokens` records the per-request
limit Pig Pen asks the provider to apply. `max_world_tool_calls_per_turn`
records the request cap enforced by Scry (invalid requests also count). Everything needed to describe
the run is in this line, although model sampling is not guaranteed to be
reproducible, even with a sampling seed.

## `tool`

One line per successfully decoded reflected world-tool invocation. Calls Scry
rejects during admission, budget, protocol, or schema validation do not enter Pig Pen's world
layer and therefore do not produce a `tool` record.

```json
{"type":"tool","turn":1,"tick":2,"tool":"look",
 "scry_turn_id":1,"call_id":"call_2","round":1,"index":1,
 "args":{"direction":"south"},
 "result":{"cells":[{"distance":1,"item":"berry"}],"direction":"south","ok":true,"wall_at_distance":6},
 "before":{"x":5,"y":5},"after":{"x":5,"y":5},"action_executed":true,"result_dispatched":true,"score_after":0}
```

`tick` is a monotonic counter across the whole episode. `before`/`after` are
the blob's position either side of the call — identical for `look`, `eat`, a
wall-blocked `move`. `action_executed` remains `true` for these world records;
refused calls never enter the feed. `scry_turn_id`, `call_id`, `round` (one-based),
and `index` (zero-based within the batch) come from Scry's contextual handler.
`turn` is Pig Pen's episode turn number. `args` and `result` are copied from
Scry's dispatch observation, exactly as posted for the provider, so a log made
with `--opaque-look` shows `"something"` here too.
`result_dispatched: false` marks a world action whose result could not be posted
because the turn failed during dispatch; `args` and `result` are then `null`,
while the typed transition and score remain truthful.

## `turn`

One line per conversation turn, flushed as it completes.

```json
{"type":"turn","turn":1,"status":"completed",
 "user_message":"Automatic turn instructions:\nContinue exploring autonomously. ... Turn 1 of 2.",
 "assistant_text":"I have explored the pen and found valuable items. ...",
 "error":"","input_tokens":1699,"output_tokens":198,"tool_calls":2,
 "zero_tool_turn":false,"latency_ms":2499,
 "scry_tools":{"rounds":1,"calls":2,"rejected_calls":0,
   "round_limit_reached":false,"unexecuted_calls":0}}
```

`status` is `completed`, `cancelled`, or `error`. `tool_calls` is counted from
successfully decoded reflected handler invocations for this turn and
`zero_tool_turn` makes narration-only or invalid-call-only turns easy to query.
`scry_tools` comes from Scry's completion: `calls` includes unknown, undecodable,
and refused requests; `rejected_calls` counts admission/budget refusals, not
decode errors. `rounds` counts dispatched rounds. `round_limit_reached` and
`unexecuted_calls` report requests dropped when the round cap completes a turn.
Those dropped calls are in neither `calls` nor the activity feed. The whole
`scry_tools` value is `null` when Scry fails/cancels without a Completion;
world `tool_calls` still records any actions already observed. A provider token
limit does return a Completion with statistics, although Pig Pen treats that
truncated response as a turn error.

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

Just the executed world-call trace:

```sh
jq -r 'select(.type=="tool")
  | "\(.tick) \(.tool) \(.args) -> \(.result | tostring[0:80])"' logs/<run>.jsonl
```

Turns containing Scry budget or admission refusals:

```sh
jq -c 'select(.type=="turn" and (.scry_tools.rejected_calls // 0) > 0)' logs/<run>.jsonl
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
