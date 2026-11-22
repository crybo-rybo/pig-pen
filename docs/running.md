# Run Pig Pen

Both front ends talk to [llamad](https://github.com/crybo-rybo/llamad). llamad
is a daemon that serves one llama.cpp model over gRPC on a Unix socket. Pig
Pen does not start llamad. Start it first.

## Start llamad

Pig Pen needs llamad 0.2.x. Build llamad from its repository. Then start it
with the GGUF model that you want to use:

```sh
llamad --model /absolute/path/to/model.gguf --ctx 32768
```

llamad prints the socket that it listens on, for example
`[llamad] listening on unix:/run/user/1000/llamad.sock`. The default socket is
`$XDG_RUNTIME_DIR/llamad.sock`. If `XDG_RUNTIME_DIR` is not set, the default
is `/tmp/llamad-<uid>.sock`. Pig Pen uses the same default. If you start
llamad with `--socket PATH`, give the same path to Pig Pen.

### The context size

llamad sets its context size (`--ctx`) when it starts. The default is 4096
tokens. Pig Pen cannot change it. llamad is stateless, so Pig Pen sends the
full conversation again in each request, and each tool round is one request.
**Pig Pen does not trim or summarize the history.** Thus, the conversation
grows in each turn, and when it no longer fits in the context, the episode
ends.

Pig Pen reads the context size when it connects. It estimates the context
that the turn budget needs: 1000 tokens, plus 400 tokens for each turn. The
estimate comes from measurements with a Qwen model: about 860 tokens before
the first reply, and up to five requests in a turn that add 70 to 100 tokens
each. It is a rough upper estimate for the default tools, not a guarantee.
Other models and talkative replies can need more. If the context is smaller
than the estimate, Pig Pen shows a warning, but it starts the episode. A
small context can be what you want to test:

```
warning: llamad runs with a context of 4096 tokens, but a turn budget of 20 can need about 9000 tokens (a rough estimate). Pig Pen does not trim the history, so the episode can end with context_exhausted. Start llamad with a larger --ctx, or use fewer turns.
```

When the context is full, the turn and the episode end with
`context_exhausted`. This occurs in two ways:

- llamad refuses a prompt that is longer than its context
  (`llamad OUT_OF_RANGE: prompt is too long: 4140 tokens for a context of
  4096`).
- A reply fills the context while llamad generates it. llamad then ends the
  reply with the finish reason `length`, the same reason as for the
  output-token limit. Pig Pen adds the prompt tokens and the output tokens of
  the request. If the sum is the context size or more, the context is full.

The CLI then stops with exit code 6. To prevent this, start llamad with a
larger `--ctx`, or use fewer turns. A 20-turn episode needs about 9000 tokens
by the estimate. Use the largest context that the model and the memory
permit. The KV cache of llamad keeps the earlier part of the conversation, so
a round decodes only its new messages.

The model must have a chat template. Pig Pen refuses an embedding model and a
model without a chat template when it connects. The socket has mode 0600, and
it is the only credential. Pig Pen sends no API key.

Pig Pen also refuses a daemon that is not llamad 0.2.x. Before version 1.0, a
minor release of llamad can change the wire contract, and Pig Pen uses the
`llamad.proto` of 0.2.0. llamad 0.1.x does not report its version. The
refusal names the socket and the version that the daemon reports:

```
startup error: llamad at /run/user/1000/llamad.sock is older than 0.2.0, but this Pig Pen needs llamad 0.2.x
startup error: llamad at /run/user/1000/llamad.sock is version 0.3.0, but this Pig Pen needs llamad 0.2.x
```

Then the CLI stops with exit code 1, and Pig Pen writes no log. The GUI shows
the same message in the **Controls** panel.

The model operates on its own. You do not write a user prompt. The system
prompt tells the blob to explore without help. For each turn, Pig Pen sends an
automatic turn instruction until the episode ends.

## Headless CLI

```sh
./build/dev/pig-pen-headless --turns 4 --seed 42
```

No option is necessary. The CLI connects to the default socket. The CLI prints
this data:

- At the start, the model description, the model file, the llamad version,
  the socket, the context size, the session settings, and the log path. If
  the context is smaller than the estimate for the turn budget, a warning
  line follows the `session` line on stderr. Refer to
  [The context size](#the-context-size).
- The assistant text, when each streamed chunk arrives.
- One line for each decoded world-tool call, with the position before and
  after the call.
- At the end, a summary.

```
session model="qwen2 0.5B Q4_K - Medium" file="qwen2-0_5b-instruct-q4_k_m.gguf" llamad=0.2.0 socket="/run/user/1000/llamad.sock" n_ctx=32768 seed=42 turns=4 max_tool_rounds=8 max_world_tool_calls_per_turn=4 max_output_tokens=8192 temperature=0 sampling_seed=unset
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

Pig Pen asks llamad to limit each response to 8192 output tokens. To change
this limit, use `--max-output-tokens`. A reply that gets to this limit ends
the episode with an error. The prompt
tells the model to call the registered world tools. It tells the model not to
use extended thinking, and not to describe the actions that it will do. For
the call budget and the round limit of each turn, refer to
[Tool errors and limits](world.md#tool-errors-and-limits).

Each request also asks llamad to turn reasoning off (`REASONING_OFF`). No
option changes this. A reasoning model, for example Qwen3, then replies
without a `<think>` block. Reasoning text would make each turn slower, and
it would go into the transcript, the log, and the history that each request
sends again. A model without reasoning is not affected. Some chat templates
always open a reasoning block. With such a model, llamad does not send the
reasoning as text, but the output tokens of the turn still count it.

### Startup timing

Before the episode starts, Pig Pen does one handshake with llamad: it asks
for the model information (`GetModelInfo`). The handshake blocks the CLI. It
has a time limit:

- If nothing listens on the socket, the handshake fails immediately.
- If a process accepts the connection but does not answer, the handshake
  fails at its time limit. The limit is 5 seconds, or the
  `--timeout-seconds` value if that is smaller.

The `--timeout-seconds` clock starts before the handshake. Thus, the time of
the handshake counts against the episode timeout. With `--timeout-seconds 1`,
the CLI stops after about one second:

```
startup error: could not reach llamad at /run/user/1000/llamad.sock: it did not answer within 1 second (llamad DEADLINE_EXCEEDED: Deadline Exceeded). The daemon can be busy or stuck. Try again, or start it again with: llamad --model MODEL.gguf --socket /run/user/1000/llamad.sock
```

A signal does not stop the handshake. If `SIGINT` or `SIGTERM` arrives
during the handshake, the CLI examines it when the handshake ends. If the
handshake fails, the CLI prints the startup error, and stops with exit code
130 or 143, not 1. If the handshake succeeds, the CLI cancels the episode as
for any other signal.

### When llamad stops sending

While llamad has a request in its queue or in its engine, it sends a chunk at
least about one time each second. When it has no text to send, it sends a
`progress` chunk. Only the slowest step of the engine can make a longer
silence: one prompt batch of up to 2048 tokens, or one token. Thus, a long
silence shows that the daemon is stuck.

Pig Pen measures the silence of each request, not its duration. A request
that waits in the queue of llamad behind the request of another client gets
`progress` chunks, so Pig Pen does not stop it. If llamad sends nothing for
`--stall-seconds` (default 120), Pig Pen stops the request. The turn ends
with the status `error`, and the episode ends with the finish reason
`error`. Pig Pen does not try again. The CLI writes the error of the turn,
and stops with exit code 1:

```
error[turn=1]: llamad sent nothing for 120 seconds, so Pig Pen stopped the request
terminal error: llamad sent nothing for 120 seconds, so Pig Pen stopped the request
```

The log footer is complete. `--stall-seconds 0` turns the check off. Then
only `--timeout-seconds` (or **Stop** in the GUI) ends a request that hangs.
On a slow CPU, one prompt batch can take 10 seconds or more. Keep
`--stall-seconds` well above that time.

### Options

| flag | default | description |
|---|---|---|
| `--socket PATH` | `$XDG_RUNTIME_DIR/llamad.sock`, else `/tmp/llamad-<uid>.sock` | the llamad Unix socket |
| `--seed INTEGER` | `0` | world seed. It sets the item positions. |
| `--turns INTEGER` | `20` | turn budget, 1 to 10000 |
| `--max-tool-rounds INTEGER` | `8` | maximum tool rounds in one turn, 1 to 64 |
| `--max-output-tokens INTEGER` | `8192` | output-token limit for each request (llamad `max_tokens`), 1 to 2147483647 |
| `--temperature NUMBER` | `0.0` | model sampling temperature, 0.0 to 2.0. It is independent of the world seed. |
| `--sampling-seed INTEGER` | *(not set)* | model sampling seed, 0 to 4294967295. Pig Pen sends it as the llamad `SamplingParams.seed` only when you give it. Without it, llamad uses a random seed. |
| `--timeout-seconds INTEGER` | `300` | wall-clock time limit for the full episode, 1 to 86400. The llamad handshake counts against it. Refer to [Startup timing](#startup-timing). |
| `--stall-seconds INTEGER` | `120` | Pig Pen stops a request when llamad sends nothing for this time, 0 to 86400. `0` turns the check off. Refer to [When llamad stops sending](#when-llamad-stops-sending). |
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
| `1` | A runtime error occurred (also a stalled request, or a failed handshake), or the episode ended with `error`, `cancelled`, or `stopped`. |
| `2` | The command line is not valid. |
| `3` | The `--timeout-seconds` time limit ended. |
| `4` | Pig Pen could not write the metrics log. |
| `5` | The episode completed without a decoded world-tool call. |
| `6` | The episode ended with `context_exhausted`: the conversation no longer fits in the llamad context. |
| `130` / `143` | `SIGINT` / `SIGTERM`, also when the signal arrived during a handshake that then failed |

For exit code 6, the CLI writes the error of the turn and one more line to
stderr. The line tells what to do:

```
error[turn=15]: llamad OUT_OF_RANGE: prompt is too long: 4140 tokens for a context of 4096
context exhausted: the conversation filled the llamad context of 4096 tokens on turn 15 of 20. Pig Pen does not trim the history. Start llamad with a larger --ctx, or use fewer --turns.
```

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
until you push **Play** or **Reset**.

**Play** and **Reset** do the llamad handshake in the frame loop. The window
does not draw or take input during the handshake. Thus, the GUI gives the
handshake a time limit of 1 second. If llamad does not answer in this time,
the **Controls** panel shows the error, and it names the limit:

```
Could not create session: could not reach llamad at /run/user/1000/llamad.sock: it did not answer within 1 second (llamad DEADLINE_EXCEEDED: Deadline Exceeded). The daemon can be busy or stuck. Try again, or start it again with: llamad --model MODEL.gguf --socket /run/user/1000/llamad.sock
```

The GUI uses the default stall limit of 120 seconds. It has no control for
it. Refer to [When llamad stops sending](#when-llamad-stops-sending). You can dock the panels. The GUI keeps the layout in
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
  shows the model description, the model file, the llamad version, and the
  context size that llamad reports.
- *Scenario*: a preset dropdown, and a checkbox for each of the three scenario
  flags. The presets are Default, Hidden values, No reward feedback, Opaque
  look, and Blind learning. If you change a checkbox, the dropdown changes to
  `Custom`. Pig Pen writes the preset name to the log as `prompt_variant`.
- The seed (with **Reroll + Reset**), the turn budget, the tool rounds for each
  turn, the maximum output tokens for each request, the temperature, an
  optional sampling seed, and the animation speed. To send the sampling seed,
  select its checkbox. Changes to the output tokens, the temperature, the
  sampling seed, and other session settings apply only after a reset.
- *Episode*: **Play / Pause / Stop / Reset**, the current state, the log path,
  the notices, and the error (if there is one). If the context of llamad is
  smaller than the estimate for the turn budget, the notices include the
  startup warning in amber. Refer to [The context size](#the-context-size).

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

An episode ends for one of six reasons. The log footer records the reason as
`finish_reason`:

| reason | when |
|---|---|
| `turn_budget` | The episode used all turns in the turn budget. |
| `objective_complete` | The blob ate all items with a positive value. Toadstools can remain. |
| `stopped` | The user pushed **Stop**, or the CLI got a signal or a timeout. |
| `cancelled` | The active model turn ended with a cancellation. |
| `error` | llamad, the model turn, or the metrics writer had a terminal failure. This includes a request that Pig Pen stopped because llamad sent nothing for the stall limit. Pig Pen does not try again. |
| `context_exhausted` | The conversation no longer fits in the llamad context. Pig Pen does not trim the history, so the episode cannot continue. Refer to [The context size](#the-context-size). |

If you close the window or reset during an episode, Pig Pen still writes a
footer. This footer has `"complete": false` and the reason `abandoned`. Pig Pen
does not leave a truncated log.
