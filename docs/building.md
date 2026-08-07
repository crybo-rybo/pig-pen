# Building

## Prerequisites

| requirement | notes |
|---|---|
| CMake 3.25+ | presets use schema version 6 |
| Ninja | the generator both presets select |
| C++23 compiler | GCC, Clang, AppleClang, or MSVC; `std::expected` is required |
| libcurl | scry's HTTP transport links against it |
| OpenGL 3.3 | only needed for the GUI target |
| GLFW platform libraries | X11 and/or Wayland development headers on Linux |
| Python 3 | optional; enables the two headless signal tests |

Everything else is pinned in `CMakeLists.txt` and fetched at configure time:
[scry](https://github.com/crybo-rybo/scry), nlohmann/json, GLFW, Dear ImGui,
and Catch2. The first configure clones them, so it needs network access and
takes a few minutes; later configures reuse `build/<preset>/_deps`.

On Arch:

```sh
sudo pacman -S --needed cmake ninja curl mesa libx11 libxrandr libxinerama libxcursor libxi wayland
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
| `PIGPEN_BUILD_TESTS` | `ON` | build `pigpen_tests` and register the CTest cases |
| `PIGPEN_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX` for pig-pen's own code only |
| `PIGPEN_SCRY_SOURCE` | *(empty)* | path to a local scry checkout instead of the pinned revision |

pig-pen builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Those
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
`CMakeLists.txt`, otherwise configure fails with an explicit error. Clearing the
cache variable (or deleting `build/dev/`) goes back to the pinned commit.

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
