# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Pig Pen drops a locally hosted LLM into a deterministic 10×10 grid world it can
only perceive through three tools (`look`, `move`, `eat`) registered with
[scry](https://github.com/crybo-rybo/scry). Two front ends — an ImGui GUI
(`pig-pen`) and a CLI (`pig-pen-headless`) — share the same world, prompt,
tools, episode runner, and JSONL logger.

Detailed docs live in `docs/` (architecture, building, running, testing, world
rules, log format) — consult them before re-deriving anything below.

## Build & test

Requires GCC 16+ (`-std=c++26 -freflection`, P2996/P3394), CMake 3.25+, Ninja,
Python 3, libcurl. Configure fails fast on any other compiler. First configure
fetches all pinned C++ deps (scry, nlohmann/json, GLFW, ImGui, Catch2) and
needs network access.

```sh
just build                 # cmake --preset dev + build (Debug)
just test                  # build, then ctest --preset dev
just build release         # the other preset
just run dev --model NAME  # build + launch GUI
just run-headless dev --model NAME --turns 4 --seed 42
```

Gotcha: `run`/`run-headless` take the preset as the **first** positional
argument — `just run-headless --model X` parses `--model` as a preset name.

Single tests:

```sh
ctest --preset dev -R world                      # by CTest name
./build/dev/pigpen_tests --list-tests            # Catch2 binary directly
./build/dev/pigpen_tests "[determinism]"         # by tag
./build/dev/pigpen_reflection_tests --list-tests # reflection-isolated binary
```

The suite needs **no model server and no network** (integration tests use
loopback stubs) and finishes in seconds. Warnings are errors
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`) for pig-pen code
only; fetched deps are `SYSTEM`.

Formatting is enforced in CI (`.github/workflows/format.yml`):

```sh
just fmt         # clang-format all C++ + ruff format the Python tests
just fmt-check   # what CI runs
just lint        # ruff check on the Python tests
```

clang-format is pinned to 22.1.8 (CI installs exactly that; use the same
major locally). `.clang-tidy` is advisory only — clang cannot parse the
`-freflection` TUs, so it runs against `src/world`, `src/ui`, and the
non-reflection agent files via `clang-tidy -p build/dev <file>`.

Conventions the formatter can't enforce: project includes are quoted and
rooted at `src/` (`"agent/config.hpp"`, never `"config.hpp"`); files are
`snake_case`, tests end `_tests`; types are `CamelCase`, interfaces take an
`I` prefix, functions/variables/constants/enumerators are `snake_case`, and
private members take a trailing underscore. No `using namespace` outside
function or TU scope.

Useful CMake options: `-DPIGPEN_BUILD_GUI=OFF` (headless-only, no GL/GLFW
toolchain), `-DPIGPEN_SCRY_SOURCE=../scry` (local scry checkout instead of the
pinned revision; cached in `build/<preset>/_deps` — delete the build dir when
switching back).

Manual end-to-end check against a real model (the suite never does this):
`./build/dev/pig-pen-headless --model NAME --turns 2 --seed 42` — exit 0 means
at least one decoded world-tool call; exit 5 means none. It writes to `logs/`
in the CWD.

## Architecture

Three layers plus two thin entry points; everything except the final transport
integration is testable without a window or a model server
(`docs/architecture.md` has the full map).

- **`src/world`** — `World` is a pure value type: seed, blob position, score,
  items, observed bitset. No JSON, no networking. Actions return typed results
  with enumerated failures; `World::dump()` is the canonical serialisation used
  by determinism tests. Same seed + same actions ⇒ same world.
- **`src/agent`** — the runtime. `Session` composes everything (world,
  conversation, scry harness with registered tools, `EpisodeRunner`,
  `MetricsWriter`) and is the **reset unit**: there is no partial reset, you
  destroy and recreate the session (that's what the GUI Reset button does).
- **`src/ui`** — `AppUi` owns the `shared_ptr<Session>`;
  `WorldAnimationState` turns the activity feed into timed steps with
  caller-supplied time, so it is tested without ImGui or a wall clock.
- **`src/app`** — `main.cpp` (GLFW/ImGui frame loop) and `headless_main.cpp`
  (argv, signals, exit codes) contain nothing testable-by-unit.

Key invariants to preserve:

- **C++26 reflection is the tool boundary.** Tool argument/result/error types
  in `tool_contract.hpp` are the model-facing contract; scry derives JSON
  Schemas from them at compile time and does all decode/encode. Adding or
  renaming an enum value changes schema, decode, and encode from the one
  declaration. `WorldTools` never touches JSON; protocol failures (unknown
  tool, undecodable args) belong to scry and never reach it. Reflection
  compiler requirements are scoped to the `pigpen_reflected_tools` /
  `pigpen_agent` libraries — don't leak them into deps or front-end TUs.
- **Nothing blocks, no background threads.** Both front ends drive
  `Session::pump()` from their own loop (GUI per frame, CLI in a sleep-1ms
  loop). Cancellation is cooperative: an episode isn't finished until the
  terminal callback arrives, which is what guarantees the JSONL footer is
  written even on SIGINT/timeout.
- **Two test seams**: `ITurnTransport` lets `EpisodeRunner` be driven by a
  scripted transport; `WorldTools` accepts/returns only reflected C++ values.
  New agent-layer code should stay testable through one of these.
- Tools hold a `weak_ptr` back to the session so late callbacks fail cleanly.
- The three scenario flags (`--hidden-values`, `--no-reward-feedback`,
  `--opaque-look`) change only what the model is told — the world, scoring,
  and log always record the truth.
