# pig-pen

`pig-pen` is a C++23 application that lets a local LLM inhabit a deterministic
10×10 world. The model sees and changes the pen only through `look`, `move`,
and `eat` tools registered with [scry](https://github.com/crybo-rybo/scry).
Humans get the omniscient view: the grid, fog-of-war, streamed model output,
every tool result, live stats, and a JSONL experiment log.

The tested headless runtime and the Dear ImGui application share the same
world, prompt, tools, bounded episode runner, and metrics writer.

## Build

Prerequisites are CMake 3.25+, Ninja, a C++23 compiler, libcurl, OpenGL 3, and
the platform development libraries GLFW needs. All C++ dependencies are pinned
and fetched by CMake.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For development against a sibling scry checkout:

```sh
cmake --preset dev -DPIGPEN_SCRY_SOURCE=../scry
```

The equivalent shortcuts are `just build`, `just test`, `just run`, and
`just run-headless`.

## Run headless

Start Ollama and make sure the default model is installed:

```sh
ollama pull qwen3:8b
./build/dev/pig-pen-headless --turns 4 --seed 42
```

No user prompt is required. The system prompt makes the blob explore on its
own, while each episode has a finite turn budget (20 by default), at most eight
tool rounds per turn, and a five-minute CLI deadline. A successful CLI run must
contain at least one model tool call.

Useful experiment switches are:

```text
--hidden-values
--no-reward-feedback
--opaque-look
--model NAME
--base-url URL
--seed N
--turns N
--max-tool-rounds N
--timeout-seconds N
--input "guidance for the next turn"
```

Run `pig-pen-headless --help` for the complete, validated interface and exit
codes. `PIGPEN_API_KEY` supplies an optional credential for compatible
endpoints. `SIGINT` and `SIGTERM` request cooperative cancellation: the CLI
continues pumping until the terminal callback finalizes the JSONL footer, then
returns the conventional status 130 or 143 respectively.

## Run the GUI

```sh
./build/dev/pig-pen
```

The default `qwen3:8b` episode starts automatically. The dockable UI includes:

- the human-visible world, observed-cell overlay, item glyphs, a blob dot, and
  sequential move/look/eat animation;
- streamed visible model output and inline tool results;
- a filterable event log;
- endpoint, model, preset, seed, finite-budget, experiment, and run controls;
- score, item, tool, turn, latency, callback, and animation stats; and
- a bottom guidance box whose text joins the next automatic turn.

Connection and scenario edits apply on Reset. Reset rebuilds the world,
conversation, scry harness, additive tool registry, and metrics log as one
session.

## Logs and comparisons

Each episode creates `logs/<timestamp>-<model>-<seed>.jsonl` containing one
header, one record per attempted tool call, per-turn records, and a footer.
Completed footers reconcile score, item counts, tool counts, turns, duration,
and terminal reason. Closing or resetting an in-flight session records an
explicit incomplete/abandoned footer instead of pretending the episode
completed.

Inspect a run with:

```sh
jq -s '{header: first, footer: last, tools: [.[] | select(.type == "tool")]}' \
  logs/<run>.jsonl
```

For a fair experiment, keep the seed and turn budget fixed and change only one
axis:

```sh
./build/dev/pig-pen-headless --seed 2026 --turns 10 \
  --prompt-variant known-feedback
./build/dev/pig-pen-headless --seed 2026 --turns 10 --hidden-values \
  --prompt-variant hidden-feedback
./build/dev/pig-pen-headless --seed 2026 --turns 10 --no-reward-feedback \
  --prompt-variant known-no-feedback
```

Repeat the commands with another installed model and compare footer score and
tool counts plus the ordered tool records. World placement and action replay
are deterministic for a given seed.

### Recorded same-seed example

On 2026-08-06, the two-turn seed-42 matrix below was run locally against
`qwen3:8b` and `qwen3:0.6b`. “Hidden/no feedback” combines axes 1 and 2 by
adding `--hidden-values --no-reward-feedback`; all four logs ended with a
complete `turn_budget` footer.

| model | scenario | score | look / move / eat | invalid calls |
|---|---|---:|---:|---:|
| `qwen3:8b` | known + feedback | 1 | 4 / 1 / 1 | 0 |
| `qwen3:8b` | hidden + no feedback | 1 | 4 / 1 / 1 | 0 |
| `qwen3:0.6b` | known + feedback | 0 | 3 / 0 / 6 | 6 |
| `qwen3:0.6b` | hidden + no feedback | 0 | 4 / 0 / 2 | 2 |

The 8B model found, moved onto, and explicitly ate the same berry in both
conditions, so this short horizon shows no reward-information effect. The
0.6B model repeatedly supplied forbidden arguments to `eat` and never moved;
its result is evidence of tool-schema reliability dominating the experiment,
not evidence about reward learning. Longer runs and additional seeds are
needed before drawing a behavioral conclusion.

The implementation decisions and test gates are in
[IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md); the complete product
specification remains [DESIGN.md](docs/DESIGN.md).
