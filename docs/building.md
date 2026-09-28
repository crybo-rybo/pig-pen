# Building

## Prerequisites

| requirement | notes |
|---|---|
| CMake 3.31+ | presets use schema version 6 |
| Ninja | the generator the presets select |
| GCC 16+ | C++26 P2996/P3394 reflection; configured with `-std=c++26 -freflection` |
| Python 3 | required when the test suite is enabled; drives public-boundary integration tests |
| libcurl | scry's HTTP transport links against it |
| OpenGL 3.2+ | only needed for the GUI target; SDL3 creates the platform context |

Everything else is pinned and fetched at configure time from
`cmake/dependencies.cmake` or `cmake/testing.cmake`:
[scry](https://github.com/crybo-rybo/scry), nlohmann/json, SDL3, Dear ImGui, and
Catch2. The first configure clones them, so it needs network access and takes a
few minutes; later configures reuse `build/<preset>/_deps`.

On Arch:

```sh
sudo pacman -S --needed gcc cmake ninja python curl libgl mesa libx11 libxcursor libxext libxfixes libxi libxinerama libxkbcommon libxrandr libxrender libxss libxtst wayland wayland-protocols
```

On Ubuntu 24.04 (and GitHub Actions). GCC 16 is not in the default 24.04
repos; add the toolchain PPA first:

```sh
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  g++-16 ninja-build pkg-config python3 python3-pip \
  libcurl4-openssl-dev libgl1-mesa-dev \
  libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
  libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev \
  libxrender-dev libxss-dev libxtst-dev \
  libwayland-dev wayland-protocols
# Ubuntu 24.04 packages CMake 3.28; Pig Pen needs 3.31+.
python3 -m pip install --user --break-system-packages 'cmake>=3.31'
```

## macOS

Pig Pen uses two compilers on macOS. Homebrew GCC 16 compiles the C++26 code,
while Apple Clang compiles SDL3's C and Objective-C platform implementation.
CMake manages the target boundary; select the compilers on the first configure:

```sh
brew install gcc cmake ninja just
CC=/usr/bin/cc CXX=g++-16 cmake --preset dev --fresh
cmake --build --preset dev
ctest --preset dev
```

SDL3 keeps Cocoa behind its C API, so Apple framework headers are not parsed by
the GCC-compiled ImGui backend or application. The project still uses Apple's
deprecated OpenGL implementation for rendering; the build suppresses that SDK
deprecation diagnostic until the renderer is migrated separately. macOS is
validated locally but is not currently exercised by hosted CI.

## Presets

```sh
cmake --preset dev            # Debug, GUI + tests, warnings as errors
cmake --build --preset dev
```

`release` is the same configuration with `CMAKE_BUILD_TYPE=Release`. `headless`
is Debug with `PIGPEN_BUILD_GUI=OFF` (no SDL3, ImGui, or OpenGL). Each writes
to `build/<preset>/` and exports `compile_commands.json`.

Binaries land in `build/<preset>/`:

- `pig-pen` — the ImGui application (not built by the `headless` preset)
- `pig-pen-headless` — the CLI
- `pig-pen-worker` — the RL rollout worker (see [Training](training.md))
- `pigpen_tests` — the Catch2 test binary
- `pigpen_reflection_tests` — the reflection-isolated Catch2 test binary

## justfile recipes

The `justfile` wraps the presets and configures first, so a bare `just run`
works from a clean checkout. The first positional argument of every recipe is
the preset name; anything after it is forwarded to the binary.

```sh
just build                     # configure + build the dev preset
just build release
just test                      # build, then ctest --preset dev
just run dev --model YOUR_MODEL  # build, launch, and auto-start the GUI
just run-headless dev --model YOUR_MODEL --turns 4 --seed 42
just run-worker dev --model YOUR_MODEL --seeds 1-4 --samples 2
just ci                       # format + lint + dev/release/headless builds & tests (what GitHub runs)
```

Note the explicit `dev` in the `run` / `run-headless` / `run-worker` lines —
those recipes take the preset first, so `just run-headless --model YOUR_MODEL`
would be read as a preset named `--model`.

## CMake options

| option | default | effect |
|---|---|---|
| `PIGPEN_BUILD_GUI` | `ON` | build `pig-pen`; turn off to skip SDL3, ImGui, and OpenGL entirely |
| `PIGPEN_BUILD_TESTS` | `ON` | build both Catch2 test binaries and register all CTest cases |
| `PIGPEN_WARNINGS_AS_ERRORS` | `ON` | `-Werror` for pig-pen's own code only |
| `PIGPEN_SCRY_SOURCE` | *(empty)* | deprecated compatibility alias for a local scry checkout |

The presets set the first three explicitly, so re-running a preset restores
them even over a cache configured with them off.

Pig Pen is intentionally a reflection-first C++26 application. Configuration
rejects non-GNU compilers and GCC versions older than 16. Scry performs an
additional compile probe for the exact P2996/P3394 annotation-query surface
Pig Pen uses.

Pig Pen builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Those
flags apply to pig-pen sources only; fetched dependencies are added as `SYSTEM`
and keep their own warning settings.

A headless-only build (no GUI toolchain needed):

```sh
cmake --preset headless
cmake --build --preset headless
```

## Working against a local scry

FetchContent's built-in override points any dependency at a local checkout, so
no Pig Pen-specific option is needed:

```sh
cmake --preset dev \
  -DFETCHCONTENT_SOURCE_DIR_SCRY=/absolute/path/to/scry
```

Use an absolute path; CMake warns that relative `FETCHCONTENT_SOURCE_DIR_*`
values depend on the calling scope. The checkout must provide the `scry::scry`
target and pass Scry's GCC 16 capability probe; otherwise configure fails.
The deprecated `PIGPEN_SCRY_SOURCE` option is still accepted as an alias
(relative paths resolve against the project root) with a deprecation warning.
The override is stored in the preset's CMake cache; delete `build/dev/` to go
back to the pinned commit.

## Troubleshooting

**Configure hangs or fails on the first run.** It is cloning dependencies.
Check network access and proxy settings; `GIT_PROGRESS` output shows in the
configure log.

**SDL3 cannot enable a Linux display backend.** Install the X11 and/or Wayland
development headers listed above, delete `build/dev/`, and reconfigure so
SDL3's feature detection reruns.

**A dependency looks stale after using a local-source override or changing a
pinned tag.**
`FetchContent` caches under `build/<preset>/_deps`. Remove that directory or the
whole build directory and reconfigure.

**CMake still reports an older compiler after installing GCC 16.** Compiler
selection is cached per build directory. Reconfigure from a fresh directory,
or run `cmake --preset dev --fresh -DCMAKE_CXX_COMPILER=g++-16` on systems that
install versioned compiler executables.
