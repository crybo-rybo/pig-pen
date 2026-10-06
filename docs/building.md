# Build Pig Pen

## Prerequisites

| requirement | notes |
|---|---|
| CMake 3.31 or later | The presets use schema version 6. |
| Ninja | The presets use the Ninja generator. |
| GCC 16 or later | Necessary for C++26 P2996/P3394 reflection. The build uses `-std=c++26 -freflection`. |
| Python 3 | Necessary when the test suite is on. Python runs the integration tests at the public boundary. |
| libcurl | The Scry HTTP transport links to libcurl. |
| OpenGL 3.2 or later | Necessary only for the GUI target. SDL3 creates the platform context. |

The configure step gets all other dependencies at pinned versions.
`cmake/dependencies.cmake` and `cmake/testing.cmake` specify these
dependencies: [Scry](https://github.com/crybo-rybo/scry), SDL3, Dear ImGui,
and, for the tests, Catch2 and nlohmann/json. The first configure step clones
them. Network access is necessary for this step, and it takes some minutes.
Later configure steps use the files in `build/<preset>/_deps` again.

To install the packages on Arch Linux, use this command:

```sh
sudo pacman -S --needed gcc cmake ninja python curl libgl mesa libx11 libxcursor libxext libxfixes libxi libxinerama libxkbcommon libxrandr libxrender libxss libxtst wayland wayland-protocols
```

To install the packages on Ubuntu 24.04 (also used by GitHub Actions), use the
commands below. The default Ubuntu 24.04 repositories do not have GCC 16, so
the commands add the toolchain PPA first:

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
# Ubuntu 24.04 has CMake 3.28. Install CMake 3.31 or later with pip.
python3 -m pip install --user --break-system-packages 'cmake>=3.31'
```

## macOS

On macOS, Pig Pen uses two compilers:

- Homebrew GCC 16 compiles the C++26 code.
- Apple Clang compiles the SDL3 C and Objective-C platform code.

CMake controls the boundary between the targets. Select the compilers on the
first configure step:

```sh
brew install gcc cmake ninja just
CC=/usr/bin/cc CXX=g++-16 cmake --preset dev --fresh
cmake --build --preset dev
ctest --preset dev
```

SDL3 keeps Cocoa behind its C API. Because of this, GCC does not parse the
Apple framework headers when it compiles the ImGui backend and the application.
Pig Pen still uses the deprecated Apple OpenGL implementation to render. The
build suppresses that SDK deprecation diagnostic until a separate change
migrates the renderer. The developers test macOS on local computers. The hosted
CI does not test macOS at this time.

## Presets

```sh
cmake --preset dev            # Debug, GUI + tests, warnings as errors
cmake --build --preset dev
```

The `release` preset is the same as `dev`, but with
`CMAKE_BUILD_TYPE=Release`. The `headless` preset is a Debug build with
`PIGPEN_BUILD_GUI=OFF` (no SDL3, ImGui, or OpenGL). Each preset writes to
`build/<preset>/` and exports `compile_commands.json`.

The build puts these binaries in `build/<preset>/`:

- `pig-pen`: the ImGui application. The `headless` preset does not build it.
- `pig-pen-headless`: the CLI.
- `pigpen_tests`: the Catch2 test binary.
- `pigpen_reflection_tests`: the Catch2 test binary that isolates the
  reflection tests.

## justfile recipes

The `justfile` recipes use the presets. Each recipe does the configure step
first, so `just run` works in a clean checkout. The first positional argument
of each recipe is the preset name. The recipe sends all other arguments to the
binary.

```sh
just build                     # configure + build the dev preset
just build release
just test                      # build, then ctest --preset dev
just run dev --model YOUR_MODEL  # build, start the GUI, and start the episode
just run-headless dev --model YOUR_MODEL --turns 4 --seed 42
just ci                       # format + lint + dev/release/headless builds and tests (the GitHub checks)
```

CAUTION: The `run` and `run-headless` recipes take the preset as the first
argument. Always give the preset first. If you type
`just run-headless --model YOUR_MODEL`, the recipe reads `--model` as a preset
name.

## CMake options

| option | default | effect |
|---|---|---|
| `PIGPEN_BUILD_GUI` | `ON` | Builds `pig-pen`. Set it to `OFF` to build without SDL3, ImGui, and OpenGL. |
| `PIGPEN_BUILD_TESTS` | `ON` | Builds the two Catch2 test binaries and registers all CTest cases. |
| `PIGPEN_WARNINGS_AS_ERRORS` | `ON` | Uses `-Werror` for Pig Pen code only. |
| `PIGPEN_SCRY_SOURCE` | *(empty)* | Deprecated. An alias for a local Scry checkout. It stays for compatibility. |

The presets set the first three options explicitly. When you run a preset
again, it sets these options back to the preset values. This also occurs when
the cache has these options set to `OFF`.

Pig Pen uses C++26 reflection only in `pigpen_agent`. All other code builds as
C++23 (refer to [Architecture](architecture.md#build-layout)). But
`pigpen_agent` and Scry need GCC 16. Because of this, the configure step does
not accept compilers that are not GNU, or GCC versions before 16. Scry also
does a compile probe for the exact P2996/P3394 annotation-query features that
Pig Pen uses.

Pig Pen builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. These
flags apply only to Pig Pen source files. The build adds the fetched
dependencies as `SYSTEM`, and they keep their own warning settings.

To build only the CLI, use the commands below. A GUI toolchain is not
necessary.

```sh
cmake --preset headless
cmake --build --preset headless
```

## Use a local Scry checkout

The FetchContent override can point a dependency to a local checkout. A Pig Pen
option is not necessary:

```sh
cmake --preset dev \
  -DFETCHCONTENT_SOURCE_DIR_SCRY=/absolute/path/to/scry
```

Use an absolute path. If the path is relative, CMake gives a warning, because
relative `FETCHCONTENT_SOURCE_DIR_*` values depend on the calling scope. If
the checkout does not supply the `scry::scry` target, or does not pass the Scry
GCC 16 capability probe, the configure step fails.

The configure step also accepts the deprecated `PIGPEN_SCRY_SOURCE` option as
an alias, and gives a deprecation warning. With this option, a relative path
starts at the project root. The preset CMake cache keeps the override. To use
the pinned commit again, delete `build/dev/`.

## Troubleshooting

**The configure step does not finish, or it fails on the first run.** The
configure step clones the dependencies. Examine the network access and the
proxy settings. The configure log shows the `GIT_PROGRESS` output.

**SDL3 cannot enable a Linux display backend.**

1. Install the X11 development headers, the Wayland development headers, or
   both. The package lists above include them.
2. Delete `build/dev/`.
3. Do the configure step again, so that SDL3 does its feature detection again.

**A dependency is old after you use a local source override or change a
pinned tag.** `FetchContent` keeps a cache in `build/<preset>/_deps`. Remove
that directory or the full build directory. Then do the configure step again.

**CMake shows an older compiler after you install GCC 16.** CMake keeps the
compiler selection in the cache of each build directory. Configure again in a
new directory. On systems that install compiler executables with version
names, you can also run
`cmake --preset dev --fresh -DCMAKE_CXX_COMPILER=g++-16`.
