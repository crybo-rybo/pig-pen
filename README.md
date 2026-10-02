# pig-pen

Pig Pen puts a locally hosted LLM into a deterministic 10×10 pen. You can watch
the model while it plays. In the pen, the model is a blob. The blob can get data
about the pen only through three tools: `look`, `move`, and `eat`. Pig Pen
registers these tools with [Scry](https://github.com/crybo-rybo/scry).

You see all of the pen:

- the full grid
- the cells that the model has observed (its fog of war)
- the streamed model output
- each decoded world-tool call and its result
- live statistics
- a JSONL log of the run

Two front ends use the same world, prompt, tools, episode runner, and logger:

| binary | description |
|---|---|
| `pig-pen` | Dear ImGui desktop app. It shows the grid, the animation, the transcript, the event log, and the controls. |
| `pig-pen-headless` | Command-line interface (CLI) for scripts. It runs one bounded episode and then stops. |

## The pen

- The pen is a 10×10 grid with walls on all sides. Coordinates are 0 to 9.
  `(0,0)` is the south-west corner.
- The blob starts at `(5,5)`.
- The seed sets the positions of 13 items: 6 berries (**+1**), 3 apples
  (**+3**), 1 truffle (**+10**), and 3 toadstools (**−5**).
- `look(direction)` scans all cells to the wall. `move(direction)` moves the
  blob one cell. `eat()` eats the item in the current cell. When the blob moves
  over an item, it does *not* collect that item.
- The same seed and the same actions always make the same world.

## Quick start

Before you build Pig Pen, install these tools:

- GCC 16 or later (for C++26 reflection)
- CMake 3.31 or later
- Ninja
- libcurl
- Python 3 (for the tests)
- OpenGL 3.2 or later (for the GUI)

CMake gets all other dependencies. For the setup on each platform (macOS
also), refer to [Build Pig Pen](docs/building.md).

1. Start a model server. The default endpoint is Ollama on `127.0.0.1:11434`.
   Pig Pen does not select a model for you. Pull the model that you want to use:

   ```sh
   ollama pull YOUR_MODEL
   ```

2. Build and start the GUI. Give the exact model identifier. The GUI puts this
   identifier in its model field and starts the episode automatically:

   ```sh
   just run dev --model YOUR_MODEL
   ```

   To run one short episode in the terminal, use this command:

   ```sh
   just run-headless dev --model YOUR_MODEL --turns 4 --seed 42
   ```

If you do not use `just`, use these commands:

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/pig-pen --model YOUR_MODEL
./build/dev/pig-pen-headless --model YOUR_MODEL --turns 4 --seed 42
```

If you start the GUI without `--model`, the GUI waits until you select a model.
Each episode writes a log file, `logs/<timestamp>-<model>-<seed>.jsonl`. You
can examine this file after the episode.

## Documentation

- [Build Pig Pen](docs/building.md): presets, CMake options, and dependencies
- [Run Pig Pen](docs/running.md): CLI options, exit codes, and GUI panels
- [Test Pig Pen](docs/testing.md): the test suite runs without a model or a network
- [World and tools](docs/world.md): grid rules, tool schemas, and result JSON
- [Logs](docs/logs.md): the JSONL record format and `jq` examples
- [Architecture](docs/architecture.md): how the parts work together
