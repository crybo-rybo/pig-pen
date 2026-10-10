# Logs

Each session in the CLI or the GUI writes one JSONL file:

```
logs/<timestamp>-<model>-<seed>.jsonl
```

An example is `logs/20260807-064512-538-acme_pig-model_Q4_K_M-42.jsonl`. The
timestamp is in local time, with milliseconds. Pig Pen replaces characters that
are not easy to use in file names with `_`. If a file with the same name
exists, Pig Pen adds a suffix: `-1`, `-2`, and so on. Pig Pen never overwrites
a log. To write to a directory other than `logs/`, use `--log-dir`.

Each file has exactly one `header` line at the start and one `footer` line at
the end. Between them, the file has `tool` and `turn` records in the sequence
in which they occurred. Pig Pen writes object keys in sorted order. The
examples below put the keys in groups to make them easier to read.

## `header`

Pig Pen writes the header when it creates the session, before it connects to
the model.

```json
{"type":"header","model":"acme/pig-model:Q4_K_M","base_url":"http://127.0.0.1:11434/v1",
 "temperature":0.2,"max_output_tokens":8192,"seed":42,"sampling_seed":null,"started_at":"2026-08-07T06:45:12-0400","prompt_variant":"default",
 "scenario":{"grid":{"width":10,"height":10},"spawn":{"x":5,"y":5},
   "items":{"berry":6,"apple":3,"truffle":1,"toadstool":3},
   "turn_budget":2,"max_tool_rounds":8,"max_world_tool_calls_per_turn":4,
   "known_item_values":true,"reward_feedback":true,"opaque_look":false}}
```

| field | description |
|---|---|
| `prompt_variant` | The value of `--prompt-variant`, or the name of the GUI preset. |
| `temperature` | The model sampling temperature. Pig Pen records it separately from the deterministic world `seed`. |
| `sampling_seed` | The provider sampling seed from `--sampling-seed` or the GUI. If Pig Pen did not send a sampling seed, the value is `null`. |
| `max_output_tokens` | The token limit for each request. Pig Pen asks the provider to apply this limit. |
| `max_world_tool_calls_per_turn` | The request limit that Scry enforces. Requests that are not valid also count. |

This line has all the data that is necessary to describe the run. But model
sampling can give different results when you run it again, also with a
sampling seed.

## `tool`

Pig Pen writes one line for each decoded world-tool call. Some calls do not get
to the Pig Pen world layer, because Scry rejects them during admission, budget,
protocol, or schema validation. These calls do not make a `tool` record.

```json
{"type":"tool","turn":1,"tick":2,"tool":"look",
 "scry_turn_id":1,"call_id":"call_2","round":1,"index":1,
 "args":{"direction":"south"},
 "result":{"cells":[{"distance":1,"item":"berry"}],"direction":"south","ok":true,"wall_at_distance":6},
 "before":{"x":5,"y":5},"after":{"x":5,"y":5},"action_executed":true,"result_dispatched":true,"score_after":0}
```

| field | description |
|---|---|
| `tick` | A counter that increases across the full episode. |
| `turn` | The Pig Pen episode turn number. |
| `before`, `after` | The position of the blob before and after the call. These values are the same for `look`, for `eat`, and for a `move` that a wall blocks. |
| `action_executed` | Always `true` for these world records. Refused calls never go into the activity feed. |
| `scry_turn_id`, `call_id`, `round`, `index` | Values from the Scry contextual handler. `round` starts at one. `index` starts at zero in the batch. |
| `args`, `result` | Copies from the Scry dispatch observation, exactly as Scry posted them for the provider. For example, a log with `--opaque-look` also shows `"something"` here. |
| `result_dispatched` | `false` for a world action whose result Scry could not post, because the turn failed during dispatch. Then `args` and `result` are `null`, but the typed transition and the score are still correct. |

## `turn`

Pig Pen writes one line for each conversation turn. It flushes the line when
the turn completes.

```json
{"type":"turn","turn":1,"status":"completed",
 "user_message":"Automatic turn instructions:\nContinue exploring autonomously. ... Turn 1 of 2.",
 "assistant_text":"I have explored the pen and found valuable items. ...",
 "error":"","input_tokens":1699,"output_tokens":198,"attempts":2,"tool_calls":2,
 "zero_tool_turn":false,"latency_ms":2499,
 "scry_tools":{"rounds":1,"calls":2,"rejected_calls":0,
   "round_limit_reached":false,"unexecuted_calls":0}}
```

| field | description |
|---|---|
| `status` | `completed`, `cancelled`, or `error`. |
| `error` | Empty for a completed turn. For a provider failure, the Scry message, then the HTTP status, the sanitized provider error code, and the provider request ID when they are available. Example: `provider rejected the request (HTTP 404, openai:model_not_found)`. |
| `tool_calls` | The number of reflected handler calls in this turn that Scry decoded successfully. |
| `zero_tool_turn` | `true` for a turn with only narration, or with only calls that are not valid. Use it to find these turns easily in a query. |
| `input_tokens`, `output_tokens` | Token counts from the provider. |
| `attempts` | The number of provider requests in the turn, including retries. If the turn fails, this is the number of the request that failed. It is 0 if no request was attempted, or `null` when Scry does not report the count. Scry v0.7.0 omits the count for cancelled turns, even if requests were sent. |
| `latency_ms` | The duration of the turn. Pig Pen measures it locally. |
| `scry_tools` | Statistics from the Scry completion. Refer to the table below. |

The `scry_tools` value has these fields:

| field | description |
|---|---|
| `calls` | All requests. This count includes unknown requests, undecodable requests, and refused requests. |
| `rejected_calls` | Admission refusals and budget refusals. This count does not include decode errors. |
| `rounds` | The number of dispatched rounds. |
| `round_limit_reached`, `unexecuted_calls` | The requests that Scry dropped when the round limit completed the turn. These dropped calls are not in `calls`, and they are not in the activity feed. |

If Scry fails or cancels without a Completion, the full `scry_tools` value is
`null`. In that case, `tool_calls` still records the world actions that Pig Pen
observed before. A provider token limit gives a Completion with statistics. But
Pig Pen sees that truncated response as a turn error.

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

To find the turns that have Scry budget refusals or admission refusals, use
this command:

```sh
jq -c 'select(.type=="turn" and (.scry_tools.rejected_calls // 0) > 0)' logs/<run>.jsonl
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
