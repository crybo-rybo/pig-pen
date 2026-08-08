# Running

Both front ends talk to an OpenAI-compatible chat endpoint. The default
endpoint is Ollama on `http://127.0.0.1:11434/v1`, but there is no default
model. Serve a model and use its exact identifier:

```sh
ollama serve          # if it is not already running
ollama pull YOUR_MODEL
```

Any other OpenAI-compatible server works too — point `--base-url` (CLI) or the
**Base URL** field (GUI) at it. `PIGPEN_API_KEY`, if set in the environment,
is sent as the credential; local Ollama does not need it.

The model drives itself. There is no user prompt to write: the system prompt
tells the blob to explore on its own, and each turn is nudged forward
automatically until the episode ends.

## Headless CLI

```sh
./build/dev/pig-pen-headless --model YOUR_MODEL --turns 4 --seed 42
```

`--model` is required. Pig Pen does not discover, normalize, alias, or default
model names; the value is forwarded directly to the configured server:

```sh
./build/dev/pig-pen-headless --model llama3.1:8b-instruct-q4_K_M
```

It prints the session settings and log path up front, streams the assistant
text as it arrives, prints one line per tool call with the before → after
position, and ends with a summary:

```
session model="llama3.1:8b-instruct-q4_K_M" base_url="http://127.0.0.1:11434/v1" seed=42 turns=4 max_tool_rounds=8 max_output_tokens=2048 temperature=0
log_path="logs/20260807-101500-123-llama3.1_8b-instruct-q4_K_M-42.jsonl"
tool[turn=1,tick=1] look args={"direction":"north"} result={"cells":[...],"direction":"north","wall_at_distance":5} position=(5,5)->(5,5)
assistant[turn=1]: I
assistant[turn=1]:  scanned
assistant[turn=1]:  north
summary finish_reason=turn_budget turns_used=4 turn_budget=4 score=1 tool_calls=7
log_path="logs/20260807-101500-123-llama3.1_8b-instruct-q4_K_M-42.jsonl"
```

Assistant text is emitted one line per streamed chunk, so it is chatty by
design — pipe it to a file, or read the JSONL log instead, if you want the
turn's text in one piece. Tool lines go to stdout; errors, timeouts, and signal
notices go to stderr.

Each request allows at most 2048 output tokens. The prompt asks the model to
prioritize calling the registered world tools over extended thinking or
describing intended actions.

### Options

| flag | default | meaning |
|---|---|---|
| `--base-url URL` | `http://127.0.0.1:11434/v1` | model endpoint |
| `--model NAME` | *(required)* | exact model identifier forwarded to the server |
| `--seed INTEGER` | `0` | world seed; fixes item placement |
| `--turns INTEGER` | `20` | turn budget, 1–10000 |
| `--max-tool-rounds INTEGER` | `8` | tool rounds allowed inside one turn, 1–64 |
| `--temperature NUMBER` | `0.0` | model sampling temperature, 0.0–2.0; independent of the world seed |
| `--timeout-seconds INTEGER` | `300` | wall-clock deadline for the whole episode, 1–86400 |
| `--log-dir PATH` | `logs` | where the JSONL file is written |
| `--prompt-variant NAME` | `default` | free-form label stored in the log header |
| `--input TEXT` | *(none)* | human guidance appended to the first turn's nudge |
| `--hidden-values` | off | omit the item/reward table from the system prompt |
| `--no-reward-feedback` | off | hide the numeric reward and running score from `eat` results |
| `--opaque-look` | off | `look` reports occupied cells as `"something"` instead of naming the item |
| `--help` | | print the full interface and exit |

Values may be written either way: `--seed 42` or `--seed=42`. Flags in the last
block take no value.

The three scenario flags change only what the model is told — the world, the
scoring, and the log always record the truth. See
[World and tools](world.md#what-the-model-is-told) for exactly what each one
alters.

### Exit codes

| code | meaning |
|---|---|
| `0` | episode finished and the model called at least one tool |
| `1` | runtime error, or the episode ended in `error` / `cancelled` / `stopped` |
| `2` | invalid command line |
| `3` | `--timeout-seconds` elapsed |
| `4` | the metrics log could not be written |
| `5` | the episode completed without a single model tool call |
| `130` / `143` | `SIGINT` / `SIGTERM` |

`SIGINT` and `SIGTERM` request cooperative cancellation. The CLI keeps pumping
until the in-flight turn's cancellation callback arrives so the JSONL footer is
still written, then exits with the conventional status. Everything else —
timeout, metrics failure — gives cancellation 15 seconds before giving up.

## GUI

```sh
./build/dev/pig-pen --model YOUR_MODEL
```

`--model NAME` populates **Model (required)** under **Controls** and starts the
episode automatically. `--base-url URL` similarly overrides the initial
endpoint. Both accept `--option=value` syntax. If `--model` is omitted, the
window opens without an episode and waits for manual model selection. Panels
are dockable; the layout is remembered in `imgui.ini` next to the working
directory.

**World** — the whole 10×10 pen, drawn with `(0,0)` at the south-west corner.
Cells the model has never observed are tinted dark; observed cells get a teal
outline. Items are drawn as coloured glyphs, the blob is the teal dot, and
`look`/`move`/`eat` play back one action at a time so a burst of tool calls
reads as a sequence rather than a teleport. Hover a cell for its coordinates,
item, and observed state.

**Transcript** — automatic instructions, human guidance, verified world-event
actions, and model narration are labelled separately. It auto-scrolls by
default, and a narration-only turn is called out as having no verified action.

**Event Log** — every tool call as a table row (tick, turn, tool, arguments,
result), with a text filter.

**Controls** —
- *Connection*: base URL and model name.
- *Scenario*: a preset dropdown (Default, Hidden values, No reward feedback,
  Opaque look, Blind learning) plus the three individual checkboxes; toggling a
  checkbox switches the dropdown to `Custom`. The preset name is stored as the
  log's `prompt_variant`.
- Seed (with **Reroll + Reset**), turn budget, tool rounds per turn,
  temperature, and animation speed. Temperature and other session settings
  take effect only on reset.
- *Episode*: **Play / Pause / Stop / Reset**, the current state, the log path,
  and any error.

**Guidance** — messages are queued FIFO, one per model turn. Each row shows
`Pending for turn N` or `Sent on turn N`; pending rows can be removed
individually or cleared together, including while the episode is paused.

Connection and scenario edits do not touch a running episode — the panel shows
"Pending settings apply on Reset" until you press **Reset**, which rebuilds the
world, conversation, tool registry, and log as one new session. Animation speed
applies immediately.

**Stats** — score, turns used against the budget, last turn latency, finish
reason, items eaten by type, per-tool call counts, and transport/animation
queue depths.

**Guidance** — optional free text. Press Enter or **Queue** and it is appended
to the next automatic turn nudge; only the most recently queued message is
carried, and it is consumed by the turn that picks it up.

## Ending an episode

An episode finishes for one of five reasons, recorded as `finish_reason` in the
log footer:

| reason | when |
|---|---|
| `turn_budget` | the turn budget was used up |
| `objective_complete` | every positive-value item has been eaten (toadstools may remain) |
| `stopped` | **Stop**, or a CLI signal/timeout/metrics failure |
| `cancelled` | the in-flight model turn was cancelled |
| `error` | the transport or the model turn failed terminally; there is no retry |

Closing the window or resetting mid-episode still writes a footer — marked
`"complete": false` with reason `abandoned` — rather than leaving a truncated
log.
