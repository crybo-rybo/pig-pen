# Building

## Prerequisites

| requirement | notes |
|---|---|
| CMake 3.25+ | presets use schema version 6 |
| Ninja | the generator both presets select |
| GCC 16+ on Linux | C++26 P2996/P3394 reflection; configured with `-std=c++26 -freflection` |
| Python 3 | required when the test suite is enabled; drives public-boundary integration tests |
| libcurl | scry's HTTP transport links against it |
| OpenGL 3.3 | only needed for the GUI target |
| GLFW platform libraries | X11 and/or Wayland development headers on Linux |

Everything else is pinned in `CMakeLists.txt` and fetched at configure time:
[scry](https://github.com/crybo-rybo/scry), nlohmann/json, GLFW, Dear ImGui,
and Catch2. The first configure clones them, so it needs network access and
takes a few minutes; later configures reuse `build/<preset>/_deps`.

On Arch:

```sh
sudo pacman -S --needed gcc cmake ninja python curl mesa libx11 libxrandr libxinerama libxcursor libxi wayland
```

## Presets

```sh
cmake --preset dev            # Debug, GUI + tests, warnings as errors
cmake --build --preset dev
```

`release` is the same configuration with `CMAKE_BUILD_TYPE=Release`. Both write
to `build/<preset>/` and export `compile_commands.json`.

Binaries land in `build/<preset>/`:

- `pig-pen` — the ImGui application
- `pig-pen-headless` — the CLI
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
```

Note the explicit `dev` in the last line — `run` and `run-headless` take the
preset first, so `just run-headless --model YOUR_MODEL` would be read as a
preset named `--model`.

## CMake options

| option | default | effect |
|---|---|---|
| `PIGPEN_BUILD_GUI` | `ON` | build `pig-pen`; turn off to skip GLFW, ImGui, and OpenGL entirely |
| `PIGPEN_BUILD_TESTS` | `ON` | build both Catch2 test binaries and register all CTest cases |
| `PIGPEN_WARNINGS_AS_ERRORS` | `ON` | `-Werror` for pig-pen's own code only |
| `PIGPEN_SCRY_SOURCE` | *(empty)* | path to a local scry checkout instead of the pinned revision |

Pig Pen is intentionally a reflection-first C++26 application. Configuration
rejects non-GNU compilers and GCC versions older than 16. Scry performs an
additional compile probe for the exact P2996/P3394 annotation-query surface
Pig Pen uses.

Pig Pen builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Those
flags apply to pig-pen sources only; fetched dependencies are added as `SYSTEM`
and keep their own warning settings.

A headless-only build (no GUI toolchain needed):

```sh
cmake --preset dev -DPIGPEN_BUILD_GUI=OFF
cmake --build --preset dev --target pig-pen-headless
```

## Working against a local scry

```sh
cmake --preset dev -DPIGPEN_SCRY_SOURCE=../scry
```

The path is resolved relative to the source directory and must contain a
`CMakeLists.txt`. The checkout must provide its reflection component and pass
Scry's GCC 16 capability probe; otherwise configure fails. Clearing the cache
variable (or deleting `build/dev/`) goes back to the pinned commit.

## Troubleshooting

**Configure hangs or fails on the first run.** It is cloning dependencies.
Check network access and proxy settings; `GIT_PROGRESS` output shows in the
configure log.

**GLFW fails to find a display backend.** Install the X11/Wayland development
headers listed above, delete `build/dev/`, and reconfigure so GLFW's feature
detection reruns.

**A dependency looks stale after changing `PIGPEN_SCRY_SOURCE` or a pinned tag.**
`FetchContent` caches under `build/<preset>/_deps`. Remove that directory or the
whole build directory and reconfigure.

**CMake still reports an older compiler after installing GCC 16.** Compiler
selection is cached per build directory. Reconfigure from a fresh directory,
or run `cmake --preset dev --fresh -DCMAKE_CXX_COMPILER=g++-16` on systems that
install versioned compiler executables.
