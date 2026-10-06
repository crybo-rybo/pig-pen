# CLAUDE.md

This file gives guidance to Claude Code (claude.ai/code) for work with the code
in this repository.

Pig Pen puts a locally hosted LLM into a deterministic 10×10 grid world. The
LLM can perceive the world only through three tools (`look`, `move`, `eat`).
Pig Pen registers these tools with [Scry](https://github.com/crybo-rybo/scry).
Two front ends, an ImGui GUI (`pig-pen`) and a CLI (`pig-pen-headless`), use
the same world, prompt, tools, episode runner, and JSONL logger.

The `docs/` directory has the detailed documentation. Read it before you
derive data again:

- `building.md`: prerequisites, presets, CMake options, and the local Scry
  override
- `testing.md`: the suite contents, and a manual check with a real model
- `world.md`: rules, tool schemas, the call budget, and the round limit
- `logs.md`: the JSONL record format
- `running.md`: CLI options, exit codes, and GUI panels
- `architecture.md`: layers and ownership

## Build and test

GCC 16 or later (for C++26 reflection) and CMake 3.31 or later are necessary.
The first configure step fetches pinned dependencies. Network access is
necessary for this step.

```sh
just build                 # cmake --preset dev + build (Debug)
just test                  # build, then ctest --preset dev
just build release         # the other preset
just run dev --model NAME  # build + start the GUI
just run-headless dev --model NAME --turns 4 --seed 42
just ci                    # all GitHub PR checks: fmt-check + lint + dev/release/headless builds and tests
just fmt                   # clang-format all C++ + ruff format the Python tests
just fmt-check             # the CI format check
just lint                  # ruff check on the Python tests
```

CAUTION: `run` and `run-headless` take the preset as the **first** positional
argument. If you type `just run-headless --model X`, the recipe reads
`--model` as a preset name.

To run single tests, use these commands:

```sh
ctest --preset dev -R world                      # by CTest name
./build/dev/pigpen_tests --list-tests            # Catch2 binary directly
./build/dev/pigpen_tests "[determinism]"         # by tag
./build/dev/pigpen_reflection_tests --list-tests # reflection-isolated binary
```

The suite does not use a model server or a network. Warnings are errors only
for Pig Pen code. `.github/workflows/ci.yml` enforces the format with
clang-format 22.1.8. Use the same major version locally. `.clang-tidy` is
advisory. It runs only on the C++23 TUs (`src/world`, `src/core`, `src/ui`),
because clang cannot parse the `-freflection` TUs in `src/agent`.

The formatter cannot enforce these conventions:

- Write project includes in quotes, with paths that start at `src/`
  (`"core/config.hpp"`, never `"config.hpp"`).
- Use `snake_case` for file names. Test file names end with `_tests`.
- Use `CamelCase` for types. Interface names start with an `I` prefix.
- Use `snake_case` for functions, variables, constants, and enumerators.
- End private member names with an underscore.
- Do not use `using namespace` outside function scope or TU scope.

## Architecture

For the layers (`src/world`, `src/core`, `src/agent`, `src/ui`, `src/app`),
refer to `docs/architecture.md`. `Session` is the reset unit. A partial reset
is not possible.

Keep these invariants:

- **C++26 reflection is the tool boundary, and only `pigpen_agent` uses it.**
  - The annotated arguments in `tool_contract.hpp` and the plain response
    aggregates in `core/tool_responses.hpp` are the contract with the model.
    Scry derives JSON Schemas from them at compile time, and does all decode
    and encode operations.
  - When you add or rename an enum value, the schema, the decode, and the
    encode change from that one declaration.
  - `WorldTools` never uses JSON. Protocol failures (unknown tool, undecodable
    arguments) belong to Scry, and never get to `WorldTools`.
  - All other code is C++23: `pigpen_world`, `pigpen_core` (`src/core`,
    namespace `pigpen::core`: runner, prompt, `WorldTools`), `pigpen_ui`, both
    entry points, and `pigpen_tests`.
  - `pigpen_agent` (`src/agent`: toolbox, transport, reflected JSONL writer,
    `Session`) links Scry and requests C++26 privately. These settings do not
    go to the targets that link `pigpen_agent`.
  - `pigpen_core` never links Scry, so a `<scry/...>` include in
    `pigpen_core` does not compile.
  - Put code in `src/agent` only when reflection replaces hand-written shape
    code. Keep the public headers of `src/agent` C++23-parseable.
- **Application callbacks run on the pump thread.** Scry owns its I/O worker.
  Both front ends call `Session::pump()` from their own loop. The GUI calls it
  one time for each frame. The CLI calls it in a loop that sleeps for 1 ms.
  Cancellation is cooperative. An episode is not finished until the terminal
  callback arrives. This makes sure that Pig Pen writes the JSONL footer, also
  after a SIGINT or a timeout.
- **Two test seams:** With `ITurnTransport`, a scripted transport can operate
  `EpisodeRunner`. `WorldTools` takes world values and returns the plain
  response types. Make sure that you can test new agent-layer code through one
  of these seams.
- The standalone tool registry captures stable world bindings. These bindings
  exist longer than the harness. Transport destruction cancels and disconnects
  delivery.
- Scry owns call admission, the four-request limit, and the round-limit
  completion that keeps history. Calls that are not valid also count for the
  limit. `WorldTools` owns only world semantics and visibility. The exact
  activity payloads come from `on_tool_call`.
- `Config::sampling_seed` maps to the Scry `SamplingConfig::seed`. Scry sends it
  as the OpenAI-compatible `seed` only when it is set. It is independent of the
  world `seed`. The Scry contract limits it to 32 bits. The server applies it
  only on a best-effort basis.
- The three scenario flags (`--hidden-values`, `--no-reward-feedback`,
  `--opaque-look`) change only the data that Pig Pen gives to the model. The
  world, the score, and the log always record the true values.
