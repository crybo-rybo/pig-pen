# Run Pig Pen

Both front ends talk to [llamad](https://github.com/crybo-rybo/llamad). llamad
is a daemon that serves one llama.cpp model over gRPC on a Unix socket. Pig
Pen does not start llamad. Start it first.

## Start llamad

Build llamad from its repository. Then start it with the GGUF model that you
want to use:

```sh
llamad --model /absolute/path/to/model.gguf --ctx 32768
```

llamad prints the socket that it listens on, for example
`[llamad] listening on unix:/run/user/1000/llamad.sock`. The default socket is
`$XDG_RUNTIME_DIR/llamad.sock`. If `XDG_RUNTIME_DIR` is not set, the default
is `/tmp/llamad-<uid>.sock`. Pig Pen uses the same default. If you start
llamad with `--socket PATH`, give the same path to Pig Pen.

Give llamad a large context with `--ctx`. llamad is stateless, so Pig Pen
sends the full conversation again in each request, and each tool round is one
request. llamad uses 4096 tokens by default, and it refuses a prompt that is
longer than its context. That refusal ends the episode with an error, for
example `llamad INVALID_ARGUMENT: ...`. A 20-turn episode can need tens of
thousands of tokens. Use the largest context that the model and the memory
permit. The KV cache of llamad keeps the earlier part of the conversation, so
a round decodes only its new messages.

The model must have a chat template. Pig Pen refuses an embedding model and a
model without a chat template when it connects. The socket has mode 0600, and
it is the only credential. Pig Pen sends no API key.

The model operates on its own. You do not write a user prompt. The system
prompt tells the blob to explore without help. For each turn, Pig Pen sends an
automatic turn instruction until the episode ends.

## Headless CLI

```sh
./build/dev/pig-pen-headless --turns 4 --seed 42
```

No option is necessary. The CLI connects to the default socket. The CLI prints
this data:

- At the start, the model description, the socket, the context size, the
  session settings, and the log path.
- The assistant text, when each streamed chunk arrives.
- One line for each decoded world-tool call, with the position before and
  after the call.
- At the end, a summary.

```
session model="qwen2 0.5B Q4_K - Medium" socket="/run/user/1000/llamad.sock" n_ctx=32768 seed=42 turns=4 max_tool_rounds=8 max_world_tool_calls_per_turn=4 max_output_tokens=8192 temperature=0 sampling_seed=unset
log_path="logs/20260807-101500-123-qwen2_0.5B_Q4_K_-_Medium-42.jsonl"
tool[turn=1,tick=1] look args={"direction":"north"} result={"cells":[...],"direction":"north","ok":true,"wall_at_distance":5} position=(5,5)->(5,5)
assistant[turn=1]: I
assistant[turn=1]:  scanned
assistant[turn=1]:  north
summary finish_reason=turn_budget turns_used=4 turn_budget=4 score=1 tool_calls=7
log_path="logs/20260807-101500-123-qwen2_0.5B_Q4_K_-_Medium-42.jsonl"
```

The CLI prints one line for each streamed chunk of assistant text, so the
output has many lines. If you want the text of a turn in one piece, send the
output to a file or read the JSONL log. Tool lines go to stdout. Errors,
timeouts, and signal notices go to stderr.

If Pig Pen cannot reach llamad, the CLI stops with exit code 1 and a message
that names the socket. It writes no log.

Pig Pen asks llamad to limit each response to 8192 output tokens. The prompt
tells the model to call the registered world tools. It tells the model not to
use extended thinking, and not to describe the actions that it will do. For
the call budget and the round limit of each turn, refer to
[Tool errors and limits](world.md#tool-errors-and-limits).

### Options

| flag | default | description |
|---|---|---|
| `--socket PATH` | `$XDG_RUNTIME_DIR/llamad.sock`, else `/tmp/llamad-<uid>.sock` | the llamad Unix socket |
| `--seed INTEGER` | `0` | world seed. It sets the item positions. |
| `--turns INTEGER` | `20` | turn budget, 1 to 10000 |
| `--max-tool-rounds INTEGER` | `8` | maximum tool rounds in one turn, 1 to 64 |
| `--temperature NUMBER` | `0.0` | model sampling temperature, 0.0 to 2.0. It is independent of the world seed. |
| `--sampling-seed INTEGER` | *(not set)* | model sampling seed, 0 to 4294967295. Pig Pen sends it as the llamad `SamplingParams.seed` only when you give it. Without it, llamad uses a random seed. |
| `--timeout-seconds INTEGER` | `300` | wall-clock time limit for the full episode, 1 to 86400 |
| `--log-dir PATH` | `logs` | directory for the JSONL file |
| `--prompt-variant NAME` | `default` | free-form label that Pig Pen writes in the log header |
| `--input TEXT` | *(none)* | human guidance that Pig Pen adds to the automatic turn instruction of the first turn |
| `--hidden-values` | off | removes the item reward table from the system prompt |
| `--no-reward-feedback` | off | removes the numeric reward and the current score from `eat` results |
| `--opaque-look` | off | `look` shows occupied cells as `"something"`, and does not name the item |
| `--help` | | prints the full interface and stops |

You can write a value in two ways: `--seed 42` or `--seed=42`. The last four
flags (`--hidden-values`, `--no-reward-feedback`, `--opaque-look`, and
`--help`) do not take a value.

The `--seed` option sets the world. The `--sampling-seed` option sets the
sampling of llamad. These two options are independent. A sampling seed gives
repeatable results only on a best-effort basis. On one daemon, the same seed,
model, prompt, and temperature frequently give the same run again. But llamad
decodes a cached prompt prefix in a different batch split from a cold start,
so a reply can change after the daemon starts again. Results do not stay the
same across different models or llamad versions.

The three scenario flags change only the data that Pig Pen gives to the model.
The world, the score, and the log always record the true values. For the exact
effect of each flag, refer to
[What the model sees](world.md#what-the-model-sees).

### Exit codes

| code | description |
|---|---|
| `0` | The episode finished with at least one decoded world-tool call. |
| `1` | A runtime error occurred, or the episode ended with `error`, `cancelled`, or `stopped`. |
| `2` | The command line is not valid. |
| `3` | The `--timeout-seconds` time limit ended. |
| `4` | Pig Pen could not write the metrics log. |
| `5` | The episode completed without a decoded world-tool call. |
| `130` / `143` | `SIGINT` / `SIGTERM` |

`SIGINT` and `SIGTERM` start a cooperative cancellation. The CLI continues to
pump until the cancellation callback for the active turn arrives. This makes
sure that Pig Pen writes the JSONL footer. Then the CLI stops with the
conventional status. After a timeout, the CLI waits 15 seconds for the
cancellation, and then it stops. If Pig Pen cannot write the metrics log, the
episode ends immediately with the finish reason `error` and exit code 4.

## GUI

```sh
./build/dev/pig-pen --play
```

The GUI has these options:

| flag | description |
|---|---|
| `--socket PATH` | Puts the path in the **llamad socket** field of the **Controls** panel. The default is the llamad default socket. |
| `--play` | Connects to llamad and starts the episode automatically. |
| `--help` | Prints the options and stops. |

You can write `--socket PATH` or `--socket=PATH`.

If you do not give `--play`, the window opens without an episode. It waits
until you push **Play** or **Reset**. You can dock the panels. The GUI keeps the layout in
`imgui.ini` next to the working directory.

**World**: This panel shows the full 10×10 pen, with `(0,0)` at the south-west
corner. Cells that the model did not observe have a dark tint, and observed
cells have a teal outline. Colored glyphs show the items, and the teal dot is
the blob. The panel shows each `look`, `move`, and `eat` action one at a time. A burst
of tool calls shows as a sequence, not as a teleport. Put the
cursor on a cell to see its coordinates, its item, and its observed state.

**Transcript**: This panel shows automatic turn instructions, human guidance,
decoded world-tool calls, and model narration. Each type has a different label.
By default, the panel scrolls automatically. If a turn has no decoded
world-tool call, the panel shows a notice for that turn. This also applies to
a turn that has only calls that are not valid.

**Event Log**: This panel shows each decoded world-tool call as a row in a
table (tick, turn, tool, arguments, result). It has a text filter.

**Controls**: This panel has these controls:

- *Connection*: the llamad socket. After Pig Pen connects, this section also
  shows the model description and the context size that llamad reports.
- *Scenario*: a preset dropdown, and a checkbox for each of the three scenario
  flags. The presets are Default, Hidden values, No reward feedback, Opaque
  look, and Blind learning. If you change a checkbox, the dropdown changes to
  `Custom`. Pig Pen writes the preset name to the log as `prompt_variant`.
- The seed (with **Reroll + Reset**), the turn budget, the tool rounds for each
  turn, the temperature, an optional sampling seed, and the animation speed. To
  send the sampling seed, select its checkbox. Changes to the temperature, the
  sampling seed, and other session settings apply only after a reset.
- *Episode*: **Play / Pause / Stop / Reset**, the current state, the log path,
  and the error (if there is one).

**Guidance**: Pig Pen keeps guidance messages in a FIFO queue. It sends one
message for each model turn. Each row shows `Pending for turn N` or
`Sent on turn N`. You can remove pending rows one at a time or all together.
You can also do this when the episode is paused.

Changes to the connection and the scenario do not change an active episode.
Until you push **Reset**, the panel shows "Pending settings apply on Reset".
**Reset** connects to llamad again, and builds the world, the conversation,
the tools, and the log again as one new session. Changes to the animation speed apply immediately.

**Stats**: This panel shows these values:

- the score
- the turns used and the turn budget
- the latency of the last turn
- the finish reason
- the input tokens, the input tokens from the llamad KV cache, and the output
  tokens of all turns
- the items eaten, for each item type
- the call count for each tool
- the queue depths of the transport and the animation

## How an episode ends

An episode ends for one of five reasons. The log footer records the reason as
`finish_reason`:

| reason | when |
|---|---|
| `turn_budget` | The episode used all turns in the turn budget. |
| `objective_complete` | The blob ate all items with a positive value. Toadstools can remain. |
| `stopped` | The user pushed **Stop**, or the CLI got a signal or a timeout. |
| `cancelled` | The active model turn ended with a cancellation. |
| `error` | llamad, the model turn, or the metrics writer had a terminal failure. Pig Pen does not try again. |

If you close the window or reset during an episode, Pig Pen still writes a
footer. This footer has `"complete": false` and the reason `abandoned`. Pig Pen
does not leave a truncated log.
