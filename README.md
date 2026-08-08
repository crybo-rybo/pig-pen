# pig-pen

`pig-pen` drops a locally hosted LLM into a deterministic 10×10 pen and lets you
watch it play. The model is a blob that can only perceive the pen through three
tools — `look`, `move`, and `eat` — registered with
[scry](https://github.com/crybo-rybo/scry). You get the omniscient view: the
full grid, the model's fog-of-war, its streamed output, every successfully
decoded world-tool invocation and result, live stats, and a JSONL log of the
run.

Two front ends share the same world, prompt, tools, episode runner, and logger:

| binary | what it is |
|---|---|
| `pig-pen` | Dear ImGui desktop app — grid, animation, transcript, event log, controls |
| `pig-pen-headless` | scriptable CLI that runs one bounded episode and exits |

## The pen

- A walled 10×10 grid. Coordinates run 0–9, `(0,0)` is the south-west corner,
  and the blob spawns at `(5,5)`.
- Thirteen items are placed from the seed: 6 berries (**+1**), 3 apples
  (**+3**), 1 truffle (**+10**), and 3 toadstools (**−5**).
- `look(direction)` scans every cell to the wall, `move(direction)` steps one
  cell, and `eat()` consumes whatever is underfoot. Moving over an item does
  *not* collect it.
- The same seed and the same action sequence always produce the same world.

## Quick start

You need CMake 3.25+, Ninja, GCC 16+ on Linux, Python 3 for tests, libcurl,
OpenGL 3, and the platform development libraries GLFW needs. Pig Pen uses
C++26 reflection as its main tool-definition and marshalling path; CMake enables
`-std=c++26 -freflection` and rejects compilers without the required P2996 and
P3394 support. Every C++ dependency is pinned and fetched by CMake.

Serve a model first. The default endpoint is Ollama on `127.0.0.1:11434`, but
Pig Pen does not choose a model for you:

```sh
ollama pull YOUR_MODEL
```

Then build and open the app with the exact model identifier. The GUI populates
its model field and starts the episode automatically:

```sh
just run dev --model YOUR_MODEL
```

Or run one short episode in the terminal:

```sh
just run-headless dev --model YOUR_MODEL --turns 4 --seed 42
```

Without `just`, the same thing in three commands:

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/pig-pen --model YOUR_MODEL
./build/dev/pig-pen-headless --model YOUR_MODEL --turns 4 --seed 42
```

Launching the GUI without `--model` waits for a manual model selection instead.
Every episode appends a
`logs/<timestamp>-<model>-<seed>.jsonl` file you can inspect afterwards.

## Docs

- [Building](docs/building.md) — presets, CMake options, dependencies
- [Running](docs/running.md) — CLI flags, exit codes, the GUI panels
- [Testing](docs/testing.md) — the suite runs without a model or a network
- [World and tools](docs/world.md) — grid rules, tool schemas, result JSON
- [Logs](docs/logs.md) — the JSONL record format and `jq` recipes
- [Architecture](docs/architecture.md) — how the pieces fit together
