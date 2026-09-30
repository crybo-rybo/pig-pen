# Training

`pig-pen-worker` is the RL rollout worker. It plays a batch of episodes
against a trainer's OpenAI-compatible inference server and writes one JSON
line per episode (its outcome and shaped reward) to stdout. Pig Pen trains
nothing itself: the policy, the inference server, and the optimiser are the
trainer's. The worker only guarantees that what it plays is what
`pig-pen-headless` and the GUI play: the same world, prompt, tools, call
budget, round limit, and runner, with nothing that tells an episode which
front end is driving it.

Why a worker and not a `reset()/step()` API: the trainer needs the exact
tokens the policy sampled and their log-probabilities, and those live in its
own inference server. So Pig Pen keeps its role, *play episodes and report
rewards*, and labels every request so the trainer can join the two sides.

## Running a batch

```sh
./build/dev/pig-pen-worker --model YOUR_MODEL --base-url http://trainer:8000/v1 \
  --seeds 1000-1999 --samples 4 --parallel 8 --rollout-prefix run42 \
  --temperature 0.8 > rollouts.jsonl
```

or `just run-worker dev --model YOUR_MODEL --seeds 1-8 --samples 2`.

**Jobs** are the cross product of the world seeds and the samples, in
seed-major order: every sample of the first seed, then the next seed. Each
job is one episode with its own session (world, conversation, and model
client), and gets the rollout id `<prefix>/<seed>/<sample>`, for example
`run42/1003/2`; samples count from 0.

| flag | default | meaning |
|---|---|---|
| `--model NAME` | *(required)* | exact model identifier forwarded to the server |
| `--base-url URL` | `http://127.0.0.1:11434/v1` | model endpoint |
| `--seeds LIST` | *(required)* | world seeds: comma-separated seeds and inclusive `A-B` ranges, e.g. `1-3,7`; repeatable |
| `--samples INTEGER` | `1` | episodes per world seed, 1–10000 |
| `--parallel INTEGER` | `1` | episodes in flight at once, 1–256 |
| `--rollout-prefix TEXT` | `rollout` | first part of every rollout id; visible ASCII, no spaces |
| `--sampling-seed-base INTEGER` | *(unset)* | sample *k* is sent with provider sampling seed `BASE + k` |
| `--timeout-seconds INTEGER` | `300` | per-episode deadline, 1–86400 |
| `--log-dir DIR` | *(off)* | also write each episode's JSONL log here |
| `--header NAME=VALUE` | *(none)* | extra request header on every request; repeatable |
| `--reward NAME=VALUE` | *(defaults)* | override one reward weight; repeatable |
| `--help` | | print the full interface and exit |

Every `Config` flag of `pig-pen-headless` is accepted too, with the same
meaning and defaults: `--turns`, `--max-tool-rounds`, `--temperature`,
`--sampling-seed`, `--prompt-variant`, `--hidden-values`,
`--no-reward-feedback`, and `--opaque-look` (see
[Running](running.md#options)). The one exception is `--seed`: world seeds
come only from `--seeds`, and `--seed` is rejected rather than silently
ignored. `PIGPEN_API_KEY` supplies an optional credential, as for the other
front ends.

The command line is rejected (exit 2) when `--seeds` is missing, an entry is
empty, malformed, or a descending range, a seed appears twice (rollout ids
must be unique), there would be more than 1,000,000 jobs, `--samples` or
`--parallel` is 0, `--sampling-seed-base` plus the last sample index would
exceed 4294967295, both `--sampling-seed` and `--sampling-seed-base` are
given, or a `--header` names `X-Pigpen-Rollout` or `X-Pigpen-Seed`.

Without `--sampling-seed-base` the provider sampling seed is unset
(server-random), which is the normal case for training. `--sampling-seed`
sends the same seed for every job instead. The sampling seed never affects
the world: that is fixed by the world seed alone.

`--parallel` defaults to 1 because a single local server usually decodes one
request at a time; raise it to what the trainer's server batches. All
episodes run on one thread: the worker pumps up to P sessions round-robin,
each with its own model-client I/O thread, sleeping 1 ms only when a pass
over every session found nothing to do. Nothing is shared between sessions,
so there is no locking, and every callback runs on that one thread. When an
episode ends, its record is written, its session (and log) is closed, and
the next job starts.

## Joining rollouts with the trainer's server

Every request an episode makes carries two headers:

```
X-Pigpen-Rollout: run42/1003/2
X-Pigpen-Seed: 1003
```

`X-Pigpen-Rollout` is unique per episode and identical on every request of
that episode, including retries. The trainer's server logs its sampled tokens
and log-probabilities keyed by that header; the worker's `episode` record for
the same `rollout_id` supplies the reward. Group the server's requests by
rollout id, order them by arrival, and attach the record's reward to the
trajectory. `X-Pigpen-Seed` repeats the world seed so the server can group
samples of one seed (GRPO-style groups) without parsing the id.

`--header NAME=VALUE` adds more headers, such as a run tag the server routes
on. Names Pig Pen sets itself are refused on the command line; a malformed
header, or one the model client manages (`Content-Type`, `Accept`,
`Authorization`, `X-Api-Key`, `Anthropic-Version`), is rejected when the
first session is created, which aborts the batch with exit 1 before any
request or log.

## Records

stdout is pure JSONL: one `episode` record per started job, in completion
order (not job order), then one `batch` record. Each line is queued to a
bounded buffer the pump drains every pass, so a consumer can stream it and
a reader that stops consuming can never wedge the pump: once 1 MiB of
records are buffered the worker treats stdout as failed, like a closed
pipe. Keys are sorted. Diagnostics (invalid episodes, timeouts, signals,
batch errors) go to stderr only.

### `episode`

The record has the same fields as the log footer's summary (see
[Logs](logs.md#footer)), with `type` `episode`, plus `seed`, `sample`,
`sampling_seed` (`null` when unset), and `config`, the episode's full
configuration in the log header's shape. `rollout_id` is the job's rollout
id. The values match the footer except for a timed-out episode: its record
says `invalid_reason: "timeout"`, while its footer records how the
cancellation ended (`stopped` or `cancelled`), or is `abandoned` when the
cancellation stalled. Here is a real one from a one-turn episode against the test suite's
scripted server (a `move` then an empty `eat`), grouped for reading:

```json
{"type":"episode","rollout_id":"run42/1003/2","seed":1003,"sample":2,"sampling_seed":null,
 "complete":true,"finish_reason":"turn_budget","error":"","turns_used":1,"final_score":0,
 "items_eaten":{"apple":0,"berry":0,"toadstool":0,"truffle":0},
 "tool_call_counts":{"eat":1,"look":0,"move":1},
 "calls":{"budget_refused":0,"executed":2,"host_refused":0,"invalid":0},
 "reward":{"valid":true,"invalid_reason":null,"total":0.05000000000000002,
   "score":0,"explored_cells":1,"active_turns":1,"zero_tool_turns":0,
   "failed_actions":1,"invalid_calls":0,"budget_refused_calls":0,
   "unused_turns":0,"objective_complete":false,
   "terms":{"active_turn":0.1,"budget_refused_call":0.0,"explored_cell":0.05,
     "failed_action":-0.1,"invalid_call":0.0,"objective":0.0,"score":0.0,
     "unused_turn":0.0,"zero_tool_turn":0.0}},
 "reward_version":1,
 "reward_weights":{"active_turn":0.1,"budget_refused_call":-0.25,"explored_cell":0.05,
   "failed_action":-0.1,"invalid_call":-0.5,"objective":5.0,"score":1.0,
   "unused_turn":0.1,"zero_tool_turn":-1.0},
 "duration_ms":4,
 "config":{"base_url":"http://127.0.0.1:37831/v1","model":"acme/pig-model:Q4_K_M",
   "seed":1003,"sampling_seed":null,"temperature":0.8,"max_output_tokens":8192,
   "scenario":{"grid":{"height":10,"width":10},"spawn":{"x":5,"y":5},
     "items":{"apple":3,"berry":6,"toadstool":3,"truffle":1},"turn_budget":1,
     "max_tool_rounds":8,"max_world_tool_calls_per_turn":4,
     "known_item_values":true,"reward_feedback":true,"opaque_look":false}}}
```

A trainer uses `reward.total` when `reward.valid` is true and **drops the
episode otherwise**; an invalid reward is absent, not zero, and `total` is
`null`. `invalid_reason` says why:

| `invalid_reason` | when |
|---|---|
| `error` | the model turn or transport failed terminally (for example an HTTP 4xx, or a 5xx after retries); `error` has the text |
| `cancelled` | the in-flight model turn was cancelled |
| `stopped` | a signal, an aborted batch, or a failed stdout stopped the episode |
| `timeout` | `--timeout-seconds` passed; this wins over the `stopped` or `cancelled` the cancellation then produces |

A timed-out episode is cancelled cooperatively and given 15 seconds to
finish; if it does not, its record is written anyway (with `complete: false`
and reason `timeout`) and the batch moves on. A signal that arrives during
that grace period does not extend it. A signal before any timeout, by
contrast, waits for every in-flight cancellation without limit, so each
such record reflects a finished episode.

### `batch`

The last line, written once every started episode has ended:

```json
{"type":"batch","status":"completed","jobs":3,"episodes":3,"valid":3,"invalid":0,
 "not_started":0,"duration_ms":17,"error":null,"exit_code":0}
```

`status` is `completed`, `interrupted` (a signal), or `aborted` (a session
could not be created, or reporting an episode failed; `error` has the
reason). `episodes` counts the
`episode` lines written, always `valid + invalid`. `not_started` counts jobs
that never got an episode and therefore have no record: those still queued
when a signal or abort stopped the batch, plus the job whose session could
not be created. `exit_code` is the status the process exits with.

## Exit codes

| code | meaning |
|---|---|
| `0` | every episode's reward is valid |
| `1` | a session could not be created (a configuration problem such as a bad `--header` or `--log-dir`), or reporting an episode failed; the batch is aborted: in-flight episodes are cancelled cooperatively and still get their records. Reporting failures are an episode log that could not be finalized, and a stdout that cannot be written (for example the consumer closed the pipe, or stopped reading once 1 MiB of records are buffered): the worker then writes nothing more to stdout, cancels in-flight episodes cooperatively so their logs still get footers, starts no more jobs, and says so on stderr |
| `2` | invalid command line, including `--model`, `--base-url`, or `--prompt-variant` text that is not valid UTF-8; nothing is written to stdout |
| `6` | the batch completed but at least one episode's reward is invalid |
| `130` / `143` | `SIGINT` / `SIGTERM`: in-flight episodes are cancelled cooperatively and their records written, normally invalid with reason `stopped` (or `timeout` or `error` if that is how they had already ended), queued jobs are not started, and the batch record says `interrupted` |

When more than one applies, an abort or a stdout failure (1) wins over a
signal, and a signal over 6. `SIGPIPE` is ignored, so a closed pipe is a
failed write, never a fatal signal. After the batch ends the worker waits
at most one second for a stalled stdout reader to take the buffered
records, then exits; shutdown never blocks on a reader.

## Seeds, scenarios, and evaluation

The worker does nothing to stop training and evaluating on the same seeds;
that is the trainer's policy, and the convention is:

- **Pick disjoint seed ranges** for training and evaluation, for example
  train on `--seeds 0-99999` and evaluate on `--seeds 1000000-1000999`, and
  never move a seed from one to the other. The world is fully determined by
  its seed, so an evaluation seed seen in training is a memorised level.
- **Keep the scenario flags deliberate.** `--hidden-values`,
  `--no-reward-feedback`, and `--opaque-look` change what the model is told,
  not the world or the reward. Use them as curricula (a separate batch per
  condition) or as held-out evaluation conditions, never mixed silently
  within one batch; every record's `config` says which were on.
- Evaluate with `pig-pen-headless` or with the worker at temperature 0 and
  one sample per seed; both play the identical episode.

## Reward weights and offline re-weighting

The shaped reward and its defaults are described in
[World and tools](world.md#reward). `--reward NAME=VALUE` overrides a weight
for the whole batch, and every record carries the weights used
(`reward_weights`) and the reward definition (`reward_version`).

Every record also carries the raw counts, so a trainer can re-weight a batch
offline without replaying it. With `invalid_call` at −1.0 instead:

```sh
jq -c 'select(.type=="episode" and .reward.valid)
  | {rollout_id, total: (.reward.total - .reward.terms.invalid_call
                         + .reward.invalid_calls * -1.0)}' rollouts.jsonl
```

Only valid rewards may be re-weighted; an invalid one stays invalid under any
weights.

## Per-episode logs

`--log-dir DIR` makes every episode also write the JSONL log `pig-pen-headless`
writes ([Logs](logs.md)): same file naming, same records, with the rollout id
in both the header and the footer. Logs are off by default because a training
run may play thousands of episodes; the records on stdout have everything the
reward needs.
