# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Pig Pen drops a locally hosted LLM into a deterministic 10×10 grid world it can
only perceive through three tools (`look`, `move`, `eat`) registered with
[scry](https://github.com/crybo-rybo/scry). Three front ends — an ImGui GUI
(`pig-pen`), a CLI (`pig-pen-headless`), and an RL rollout worker
(`pig-pen-worker`) — share the same world, prompt, tools, episode runner, and
JSONL logger.

Detailed docs live in `docs/` — consult them before re-deriving anything:
`building.md` (prerequisites, presets, CMake options, local scry override),
`testing.md` (what the suite covers, manual real-model check), `world.md`
(rules, tool schemas, call budget and round limit, reward), `logs.md`,
`running.md`, `training.md` (the worker: flags, records, exit codes, seed
discipline), and `architecture.md` (layers and ownership).

## Build & test

GCC 16+ (C++26 reflection) and CMake 3.31+ are required; the first configure
fetches pinned dependencies and needs network access.

```sh
just build                 # cmake --preset dev + build (Debug)
just test                  # build, then ctest --preset dev
just build release         # the other preset
just run dev --model NAME  # build + launch GUI
just run-headless dev --model NAME --turns 4 --seed 42
just run-worker dev --model NAME --seeds 1-4 --samples 2 --parallel 2
just ci                    # everything GitHub runs on a PR: fmt-check + lint + dev/release/headless builds & tests
just fmt                   # clang-format all C++ + ruff format the Python tests
just fmt-check             # the CI format check
just lint                  # ruff check on the Python tests
```

Gotcha: `run`/`run-headless`/`run-worker` take the preset as the **first**
positional argument — `just run-headless --model X` parses `--model` as a
preset name.

Single tests:

```sh
ctest --preset dev -R world                      # by CTest name
./build/dev/pigpen_tests --list-tests            # Catch2 binary directly
./build/dev/pigpen_tests "[determinism]"         # by tag
./build/dev/pigpen_reflection_tests --list-tests # reflection-isolated binary
```

The suite needs no model server and no network. Warnings are errors for
pig-pen code only. Formatting is enforced by `.github/workflows/ci.yml` with
clang-format 22.1.8 (use the same major locally). `.clang-tidy` is advisory
and only runs on `src/world` and `src/ui` (clang cannot parse the
`-freflection` TUs).

Conventions the formatter can't enforce: project includes are quoted and
rooted at `src/` (`"agent/config.hpp"`, never `"config.hpp"`); files are
`snake_case`, tests end `_tests`; types are `CamelCase`, interfaces take an
`I` prefix, functions/variables/constants/enumerators are `snake_case`, and
private members take a trailing underscore. No `using namespace` outside
function or TU scope.

## Architecture

See `docs/architecture.md` for the layers (`src/world`, `src/agent`, `src/cli`,
`src/ui`, `src/app`). `Session` is the reset unit: there is no partial reset.

Key invariants to preserve:

- **C++26 reflection is the tool boundary.** Tool argument/result/error types
  in `tool_contract.hpp` are the model-facing contract; scry derives JSON
  Schemas from them at compile time and does all decode/encode. Adding or
  renaming an enum value changes schema, decode, and encode from the one
  declaration. `WorldTools` never touches JSON; protocol failures (unknown
  tool, undecodable args) belong to scry and never reach it. `scry::scry`
  carries `-freflection` publicly, so every `pigpen_agent` TU (and
  `pigpen_reflection_tests`, which links scry directly) compiles with it;
  `pigpen_agent` links scry privately, which keeps the requirement out of
  `pigpen_world`, `pigpen_cli`, `pigpen_ui`, and the front-end TUs — keep it
  that way (headers they include, such as `config.hpp`, `reward.hpp`, and
  `session_options.hpp`, must not include scry).
- **Application callbacks run on the pump thread.** Scry owns its I/O worker
  (one per session). Every front end drives `Session::pump()` from its own
  loop (GUI per frame; CLI via `EpisodeDriver::step()` in a
  sleep-1ms-when-idle loop; worker via `EpisodeBatch`, which steps up to P
  drivers round-robin on one thread and sleeps 1 ms only after a pass with no
  progress, so no locking). Cancellation is cooperative: an episode isn't
  finished until the terminal callback arrives, which is what guarantees the
  JSONL footer is written even on SIGINT/timeout. `EpisodeDriver` is the only
  copy of the deadline and 15 s cancellation-grace policy; it takes time as a
  parameter and never sleeps. `EpisodeBatch` destroys a slot's driver before
  its session.
- **`Config` is the episode, `SessionOptions` is the host.** `Config` holds
  what changes what the model sees or what the log header records;
  `SessionOptions` holds the optional log directory (none means no JSONL at
  all), prompt variant, API key, rollout id (sent as `X-Pigpen-Rollout`),
  extra request headers, and reward weights. There is no mode enum in
  `agent/`; `Session` never reads the environment (front ends pass
  `PIGPEN_API_KEY`). Shared CLI flags are registered once in
  `cli::add_config_options()`.
- **Three test seams**: `ITurnTransport` lets `EpisodeRunner` be driven by a
  scripted transport; `WorldTools` accepts/returns only reflected C++ values;
  `IDrivableEpisode` lets `EpisodeDriver` and `EpisodeBatch` be driven by
  fake episodes (the batch through a `std::function` factory).
  New agent-layer code should stay testable through one of these.
- **Records have one serialisation.** Footer, worker `episode` record, and
  worker `batch` record all come from `summary_json.cpp`; the log header and
  the episode record's `config` share `config_json()` via the internal
  `record_json.hpp` (nlohmann stays private to `pigpen_agent`). Worker stdout
  is pure JSONL, one flushed line per record, diagnostics on stderr. A worker
  timeout forces the reward invalid with reason `timeout`. Worker exit codes:
  0 all valid, 6 some invalid, 1 a session could not be created (batch
  aborted, in-flight episodes still reported; also a failed stdout, which
  is a stop request, with `SIGPIPE` ignored), 2 usage, 130/143 signal.
  User text that reaches JSON is checked for UTF-8 at the CLI boundary
  (`cli::require_utf8`), and every dump uses `dump_line()`, which replaces
  bad bytes instead of throwing.
- The standalone tool registry captures stable world bindings that outlive the
  harness. Transport destruction cancels and disconnects delivery.
- Scry owns call admission, the four-request limit (invalid calls count),
  and history-preserving round-limit completion. `WorldTools` owns only world
  semantics and visibility. Exact activity payloads come from `on_tool_call`.
- `Config::sampling_seed` maps to scry's `SamplingConfig::seed` (sent as the
  OpenAI-compatible `seed` only when set). It is independent of the world
  `seed`, 32-bit by scry's contract, and best-effort on the server side.
- The three scenario flags (`--hidden-values`, `--no-reward-feedback`,
  `--opaque-look`) change only what the model is told — the world, scoring,
  and log always record the truth.
