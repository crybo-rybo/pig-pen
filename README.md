# pig-pen

`pig-pen` drops a locally hosted LLM into a deterministic 10×10 pen and lets you
watch it play. The model is a blob that can only perceive the pen through three
tools — `look`, `move`, and `eat` — registered with
[scry](https://github.com/crybo-rybo/scry). You get the omniscient view: the
full grid, the model's fog-of-war, its streamed output, every tool call and
result, live stats, and a JSONL log of the run.

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

You need CMake 3.25+, Ninja, a C++23 compiler, libcurl, OpenGL 3, and the
platform development libraries GLFW needs. Every C++ dependency is pinned and
fetched by CMake.

Serve a model first — the default endpoint is Ollama on `127.0.0.1:11434`:

```sh
ollama pull qwen3:8b
```

Then build and run the app:

```sh
just run
```

Or run one short episode in the terminal:

```sh
just run-headless dev --turns 4 --seed 42
```

Without `just`, the same thing in three commands:

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/pig-pen          # or ./build/dev/pig-pen-headless --turns 4 --seed 42
```

The GUI starts an episode automatically; press **Pause** or **Stop** in the
Controls panel to take over. Every episode appends a
`logs/<timestamp>-<model>-<seed>.jsonl` file you can inspect afterwards.

## Docs

- [Building](docs/building.md) — presets, CMake options, dependencies
- [Running](docs/running.md) — CLI flags, exit codes, the GUI panels
- [Testing](docs/testing.md) — the suite runs without a model or a network
- [World and tools](docs/world.md) — grid rules, tool schemas, result JSON
- [Logs](docs/logs.md) — the JSONL record format and `jq` recipes
- [Architecture](docs/architecture.md) — how the pieces fit together
