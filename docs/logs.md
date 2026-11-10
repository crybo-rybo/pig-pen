# Logs

Each session in the CLI or the GUI writes one JSONL file:

```
logs/<timestamp>-<model>-<seed>.jsonl
```

An example is `logs/20260807-064512-538-qwen2_0.5B_Q4_K_-_Medium-42.jsonl`. The
model part is the model description that llamad reports. The timestamp is in
local time, with milliseconds. Pig Pen replaces characters that are not easy
to use in file names with `_`. If a file with the same name exists, Pig Pen
adds a suffix: `-1`, `-2`, and so on. Pig Pen never overwrites a log. To write
to a directory other than `logs/`, use `--log-dir`.

Each file has exactly one `header` line at the start and one `footer` line at
the end. Between them, the file has `tool` and `turn` records in the sequence
in which they occurred. Pig Pen writes object keys in sorted order. The
examples below put the keys in groups to make them easier to read.

## Format 2

This document describes format 2. The header records it as `"format": 2`.
Format 2 is the first format for llamad. Logs without a `format` field are
format 1, from the earlier OpenAI-compatible HTTP transport. Format 2 has
these changes:

- `header`: `format`, `socket`, and `model_info` are new. `model` is now the
  model description that llamad reports. `base_url` is removed.
- `tool`: `scry_turn_id`, `action_executed`, and `result_dispatched` are
  removed. Each `tool` record is a call that ran, and its payloads are always
  present.
- `turn`: `attempts` is now `requests`, and it is always a number.
  `scry_tools` is now `tool_stats`. It is always an object, and it has the new
  `invalid_calls` count. `cached_input_tokens` is new.
- `footer`: no change.

## `header`

Pig Pen writes the header when it creates the session, after it connects to
llamad and before the first turn.

```json
{"type":"header","format":2,"model":"qwen2 0.5B Q4_K - Medium",
 "model_info":{"n_ctx":32768,"n_ctx_train":32768,"n_params":494032768,"size_bytes":397808640},
 "socket":"/run/user/1000/llamad.sock",
 "temperature":0.2,"max_output_tokens":8192,"seed":42,"sampling_seed":null,"started_at":"2026-08-07T06:45:12-0400","prompt_variant":"default",
 "scenario":{"grid":{"width":10,"height":10},"spawn":{"x":5,"y":5},
   "items":{"berry":6,"apple":3,"truffle":1,"toadstool":3},
   "turn_budget":2,"max_tool_rounds":8,"max_world_tool_calls_per_turn":4,
   "known_item_values":true,"reward_feedback":true,"opaque_look":false}}
```

| field | description |
|---|---|
| `format` | The log format version. It is `2`. |
| `model` | The model description from the llamad `GetModelInfo` call. llamad serves exactly one model. |
| `model_info` | The context size that llamad uses (`n_ctx`), the context size of the training (`n_ctx_train`), the parameter count, and the tensor size in bytes. |
| `socket` | The llamad Unix socket. |
| `prompt_variant` | The value of `--prompt-variant`, or the name of the GUI preset. |
| `temperature` | The model sampling temperature. Pig Pen records it separately from the deterministic world `seed`. |
| `sampling_seed` | The sampling seed from `--sampling-seed` or the GUI. If Pig Pen did not send a sampling seed, the value is `null`. |
| `max_output_tokens` | The token limit for each request (llamad `max_tokens`). |
| `max_world_tool_calls_per_turn` | The request limit that the tool loop enforces. Requests that are not valid also count. |

This line has all the data that is necessary to describe the run. But model
sampling can give different results when you run it again, also with a
sampling seed.

## `tool`

Pig Pen writes one line for each decoded world-tool call. Some calls do not get
to the Pig Pen world layer, because the tool loop or the dispatcher rejects
them during the budget, admission, lookup, or schema validation. These calls do
not make a `tool` record.

```json
{"type":"tool","turn":1,"tick":2,"tool":"look",
 "call_id":"call_2","round":1,"index":1,
 "args":{"direction":"south"},
 "result":{"cells":[{"distance":1,"item":"berry"}],"direction":"south","ok":true,"wall_at_distance":6},
 "before":{"x":5,"y":5},"after":{"x":5,"y":5},"score_after":0}
```

| field | description |
|---|---|
| `tick` | A counter that increases across the full episode. |
| `turn` | The Pig Pen episode turn number. |
| `before`, `after` | The position of the blob before and after the call. These values are the same for `look`, for `eat`, and for a `move` that a wall blocks. |
| `call_id`, `round`, `index` | The call ID that llamad gave, and the position of the call in the turn. `round` starts at one. `index` starts at zero in the batch. |
| `args` | The canonical encoding of the decoded arguments. It can have a different spelling from the arguments that the model sent, for example without spaces. |
| `result` | The exact text of the `tool` message that Pig Pen sent back to the model. For example, a log with `--opaque-look` also shows `"something"` here. |

## `turn`

Pig Pen writes one line for each conversation turn. It flushes the line when
the turn completes.

```json
{"type":"turn","turn":1,"status":"completed",
 "user_message":"Automatic turn instructions:\nContinue exploring autonomously. ... Turn 1 of 2.",
 "assistant_text":"I have explored the pen and found valuable items. ...",
 "error":"","input_tokens":3410,"output_tokens":198,"cached_input_tokens":1650,
 "requests":2,"tool_calls":2,"zero_tool_turn":false,"latency_ms":2499,
 "tool_stats":{"rounds":1,"calls":2,"rejected_calls":0,"invalid_calls":0,
   "round_limit_reached":false,"unexecuted_calls":0}}
```

| field | description |
|---|---|
| `status` | `completed`, `cancelled`, or `error`. |
| `error` | Empty for a completed turn. For a llamad failure, `llamad <STATUS>: <message>`. Example: `llamad INVALID_ARGUMENT: prompt is longer than the context`. |
| `assistant_text` | The text of all rounds of the turn, as it was streamed. |
| `tool_calls` | The number of decoded world-tool calls in this turn. |
| `zero_tool_turn` | `true` for a completed turn with only narration, or with only calls that are not valid. Use it to find these turns easily in a query. |
| `input_tokens`, `output_tokens` | The token counts from llamad, added across all requests of the turn. Each request contains the full conversation, so `input_tokens` counts the history one time for each round. |
| `cached_input_tokens` | The input tokens that llamad used again from its KV cache, added across all requests of the turn. |
| `requests` | The number of llamad requests in the turn. One request is one round. A failed or cancelled request is included. |
| `latency_ms` | The duration of the turn. Pig Pen measures it locally. |
| `tool_stats` | The counts of the tool loop. Refer to the table below. |

The `tool_stats` value has these fields. Pig Pen always knows them, also for a
turn that failed or was cancelled.

| field | description |
|---|---|
| `rounds` | The number of rounds whose calls were dispatched. |
| `calls` | All calls in these rounds. This count includes unknown calls, undecodable calls, refused calls, and calls over the budget. |
| `rejected_calls` | Admission refusals and budget refusals. |
| `invalid_calls` | Calls for unknown tools, and calls with arguments that do not decode. |
| `round_limit_reached`, `unexecuted_calls` | The calls that did not run because the round limit completed the turn. These calls are not in `calls`, and they are not in the activity feed. |

A response that gets to the output-token limit is a turn error. The token
counts of that request are still in the record.

## `footer`

```json
{"type":"footer","complete":true,"finish_reason":"turn_budget","error":"",
 "final_score":0,"items_eaten":{"apple":0,"berry":0,"toadstool":0,"truffle":0},
 "tool_call_counts":{"eat":6,"look":3,"move":0},
 "turns_used":2,"duration_ms":3186}
```

`complete: true` shows that the episode got to a terminal state without outside
interruption. The terminal states are `turn_budget`, `objective_complete`,
`stopped`, `cancelled`, and `error`.

Sometimes the episode does not get to a terminal state. For example, the
process stops, the window closes, or the user resets the session during the
episode. In these cases, the writer still writes a footer, but with
`complete: false` and `finish_reason: "abandoned"`. A footer is final. The
writer cannot record data after the footer, and it cannot write the footer two
times.

## Read a log

To see the header, the footer, and all decoded world-tool calls together, use
this command:

```sh
jq -s '{header: first, footer: last, tools: [.[] | select(.type == "tool")]}' \
  logs/<run>.jsonl
```

To see only the trace of the world-tool calls that ran, use this command:

```sh
jq -r 'select(.type=="tool")
  | "\(.tick) \(.tool) \(.args) -> \(.result | tostring[0:80])"' logs/<run>.jsonl
```

To find the turns that have budget refusals or admission refusals, use this
command:

```sh
jq -c 'select(.type=="turn" and .tool_stats.rejected_calls > 0)' logs/<run>.jsonl
```

To see how much of each turn llamad took from its KV cache, use this command:

```sh
jq -r 'select(.type=="turn")
  | "\(.turn) \(.cached_input_tokens)/\(.input_tokens) in \(.requests) requests"' logs/<run>.jsonl
```

To compare the outcomes of many runs, use this command:

```sh
for f in logs/*.jsonl; do
  jq -c --arg f "$f" 'select(.type=="footer")
    | {run:$f, score:.final_score, reason:.finish_reason, tools:.tool_call_counts}' "$f"
done
```

The seed fully sets the world. If two runs have the same seed and turn budget,
only the actions of the model are different. You can compare the `tool`
records of the two runs directly, line by line.
