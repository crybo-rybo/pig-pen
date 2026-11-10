# CLAUDE.md

This file gives guidance to Claude Code (claude.ai/code) for work with the code
in this repository.

Pig Pen puts a locally hosted LLM into a deterministic 10×10 grid world. The
LLM can perceive the world only through three tools (`look`, `move`, `eat`).
Pig Pen joins two other projects:
[scrydantic](https://github.com/crybo-rybo/scrydantic) (C++26 reflection: tool
schemas, the strict codec, and tool binding) and
[llamad](https://github.com/crybo-rybo/llamad) (a gRPC daemon on a Unix socket
that serves one llama.cpp model). Neither project runs a tool loop. Pig Pen
owns the loop: the history, the tool rounds, the call budget, and the round
limit. Two front ends, an ImGui GUI (`pig-pen`) and a CLI
(`pig-pen-headless`), use the same world, prompt, tools, episode runner, and
JSONL logger.

The `docs/` directory has the detailed documentation. Read it before you
derive data again:

- `building.md`: prerequisites, presets, CMake options, and the local
  dependency overrides
- `testing.md`: the suite contents, and a manual check with a real model
- `world.md`: rules, tool schemas, the call budget, and the round limit
- `logs.md`: the JSONL record format
- `running.md`: how to start llamad, CLI options, exit codes, and GUI panels
- `architecture.md`: layers and ownership

## Build and test

GCC 16.2 or later (for C++26 reflection), CMake 3.31 or later, and the system
gRPC and Protobuf (with `protoc` and `grpc_cpp_plugin`) are necessary. The
first configure step fetches pinned dependencies. Network access is necessary
for this step. The CI builds in the `gcc:16.2.0` container image with the
Debian packages in `.github/workflows/ci.yml`. macOS is not supported in this
version.

```sh
just build                 # cmake --preset dev + build (Debug)
just test                  # build, then ctest --preset dev
just build release         # the other preset
just run dev --play        # build + start the GUI and the episode
just run-headless dev --turns 4 --seed 42
just ci                    # all GitHub PR checks: fmt-check + lint + dev/release/headless builds and tests (uses CC/CXX)
just fmt                   # clang-format all C++ + ruff format the Python tests
just fmt-check             # the CI format check
just lint                  # ruff check on the Python tests
```

CAUTION: `run` and `run-headless` take the preset as the **first** positional
argument. If you type `just run-headless --turns 4`, the recipe reads
`--turns` as a preset name. Both front ends connect to llamad at `--socket
PATH` (default `$XDG_RUNTIME_DIR/llamad.sock`, else `/tmp/llamad-<uid>.sock`).

To run single tests, use these commands:

```sh
ctest --preset dev -R world                      # by CTest name
./build/dev/pigpen_tests --list-tests            # Catch2 binary directly
./build/dev/pigpen_tests "[determinism]"         # by tag
./build/dev/pigpen_reflection_tests --list-tests # reflection-isolated binary
```

The suite does not use a model or external network access. A scripted fake
llamad (`tests/support/fake_llamad.*`, and the `pigpen_fake_llamad` process
for the Python tests) listens on a Unix socket in a short temporary directory.
Never put a test socket under `build/`: AF_UNIX paths are limited to about 107
bytes. Warnings are errors only for Pig Pen code.
`.github/workflows/ci.yml` enforces the format with
clang-format 22.1.8. Use the same major version locally. `.clang-tidy` is
advisory. It runs only on the C++23 TUs (`src/world`, `src/core`,
`src/llamad`, `src/ui`), because clang cannot parse the `-freflection` TUs in
`src/agent`.

The formatter cannot enforce these conventions:

- Write project includes in quotes, with paths that start at `src/`
  (`"core/config.hpp"`, never `"config.hpp"`). Test support includes start
  at `tests/` (`"support/fake_llamad.hpp"`).
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
    scrydantic derives JSON Schemas from them at compile time, and does all
    decode and encode operations.
  - When you add or rename an enum value, the schema, the decode, and the
    encode change from that one declaration.
  - `WorldTools` never uses JSON. Unknown tools and undecodable arguments
    belong to `WorldToolDispatcher` (scrydantic), and never get to
    `WorldTools`.
  - All other code is C++23: `pigpen_world`, `pigpen_core` (`src/core`,
    namespace `pigpen::core`: runner, tool loop, prompt, `WorldTools`),
    `pigpen_llamad`, `pigpen_ui`, both entry points, and `pigpen_tests`.
  - `pigpen_agent` (`src/agent`: tool contract, toolbox and dispatcher,
    reflected JSONL writer, `Session`) links scrydantic and requests C++26
    privately. These settings do not go to the targets that link
    `pigpen_agent`.
  - `pigpen_core` links neither scrydantic nor gRPC, so a `<scrydantic/...>`
    or `<grpcpp/...>` include in `pigpen_core` does not compile.
  - Put code in `src/agent` only when reflection replaces hand-written shape
    code. Keep the public headers of `src/agent` C++23-parseable.
- **Only `pigpen_llamad` uses gRPC.** `src/llamad/llamad_client.hpp` is
  gRPC-free (pimpl). The generated stubs (`pigpen_llamad_proto`, from the
  pinned `llamad.proto`) and gRPC are private to it.
- **The tool loop belongs to Pig Pen.** `core::ToolLoopTransport` owns the
  committed history (system message first), the rounds, the four-request
  budget, and the round-limit completion that keeps history without
  unanswered calls. Calls that are not valid also count for the limit. The
  history is transactional: a turn that fails or is cancelled discards its
  messages, but its world side effects stay and are logged.
  `WorldToolDispatcher` owns admission, lookup, and decode. `WorldTools` owns
  only world semantics and visibility.
- **Application callbacks and tools run on the pump thread.** gRPC reads occur
  on one worker thread for each request, and the worker only queues events.
  Both front ends call `Session::pump()` from their own loop. The GUI calls it
  one time for each frame. The CLI calls it in a loop that sleeps for 1 ms.
  Cancellation is cooperative (`ClientContext::TryCancel`). An episode is not
  finished until the terminal callback arrives. This makes sure that Pig Pen
  writes the JSONL footer, also after a SIGINT or a timeout.
- **Three test seams:** With `ITurnTransport`, a scripted transport can
  operate `EpisodeRunner`. With `IChatClient` and `IToolDispatcher`, a
  scripted chat client can operate `ToolLoopTransport`. `WorldTools` takes
  world values and returns the plain response types. Make sure that you can
  test new code through one of these seams, or through the fake llamad.
- The bound tool handlers share ownership of the world bindings. The world
  exists longer than the tools. `Session` destroys the runner, then the tool
  loop, then the client (which cancels and joins its worker), then the tools,
  then the world. `Session::create()` opens the log last, so a rejected
  config or an unreachable daemon leaves no log.
- llamad serves exactly one model. `Config` has `socket_path`, not a model
  name. The model identity comes from `GetModelInfo`, and the log header
  records it. The socket mode (0600) is the only credential.
- `Config::sampling_seed` maps to the llamad `SamplingParams.seed`. Pig Pen
  sends it only when it is set. It is independent of the world `seed`. It is
  limited to 32 bits. Repeatability is only on a best-effort basis.
- The three scenario flags (`--hidden-values`, `--no-reward-feedback`,
  `--opaque-look`) change only the data that Pig Pen gives to the model. The
  world, the score, and the log always record the true values.
