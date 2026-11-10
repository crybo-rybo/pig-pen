# Build Pig Pen

## Prerequisites

| requirement | notes |
|---|---|
| CMake 3.31 or later | The presets use schema version 6. |
| Ninja | The presets use the Ninja generator. |
| GCC 16.2 or later | Necessary for C++26 P2996/P3394 reflection. scrydantic refuses older compilers, also GCC 16 snapshots before 16.2. |
| gRPC and Protobuf for C++ | With `protoc` and `grpc_cpp_plugin`. The build generates the llamad stubs with them. They must be built with the same C++ standard library as Pig Pen (libstdc++). |
| Python 3 | Necessary when the test suite is on. Python runs the CLI tests. |
| OpenGL 3.2 or later | Necessary only for the GUI target. SDL3 creates the platform context. |

The configure step gets all other dependencies at pinned versions.
`cmake/dependencies.cmake` and `cmake/testing.cmake` specify these
dependencies:

- [scrydantic](https://github.com/crybo-rybo/scrydantic) v0.2.0
- the `llamad.proto` file of [llamad](https://github.com/crybo-rybo/llamad)
  v0.1.0. The build does not clone the llama.cpp submodule, and does not
  build llamad.
- SDL3 and Dear ImGui, for the GUI
- Catch2 and nlohmann/json, for the tests

The first configure step clones them. Network access is necessary for this
step, and it takes some minutes. Later configure steps use the files in
`build/<preset>/_deps` again.

Pig Pen does not build or start llamad. To run an episode, build llamad from
its own repository and start it. Refer to [Run Pig Pen](running.md).

### Debian 13 (trixie)

The CI uses the official `gcc:16.2.0` container image, which is based on
Debian 13. Debian has GCC 14 only, so use that image or a GCC 16.2 that you
build. To install the other packages, use these commands:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build pkg-config python3 \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  libgl1-mesa-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
  libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev libxrender-dev \
  libxss-dev libxtst-dev libwayland-dev wayland-protocols
```

To build and test in the same container as the CI, use these commands:

```sh
docker run --rm -it -v "$PWD:/src" -w /src gcc:16.2.0 bash
# then, in the container: install the packages above (without sudo), and
cmake --preset headless && cmake --build --preset headless && ctest --preset headless
```

### Arch Linux

```sh
sudo pacman -S --needed gcc cmake ninja python grpc protobuf libgl mesa libx11 libxcursor libxext libxfixes libxi libxinerama libxkbcommon libxrandr libxrender libxss libxtst wayland wayland-protocols
```

Make sure that `gcc --version` shows 16.2 or later.

### Ubuntu

The Ubuntu toolchain PPA has only GCC 16 snapshots, and scrydantic refuses
them. Use the `gcc:16.2.0` container image as described for Debian.

## macOS

This version of Pig Pen does not support macOS yet. Pig Pen must compile with
GCC and libstdc++, because Apple Clang has no C++26 reflection. But the
Homebrew gRPC and Protobuf packages are built with Apple Clang and libc++.
GCC code cannot link them. A future change can build a pinned gRPC and
Protobuf from source with the project compiler.

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
- `pigpen_fake_llamad`: the scripted fake daemon for the CLI tests.

## justfile recipes

The `justfile` recipes use the presets. Each recipe does the configure step
first, so `just run` works in a clean checkout. The first positional argument
of each recipe is the preset name. The recipe sends all other arguments to the
binary.

```sh
just build                     # configure + build the dev preset
just build release
just test                      # build, then ctest --preset dev
just run dev --play            # build, start the GUI, and start the episode
just run-headless dev --turns 4 --seed 42
just ci                        # format + lint + dev/release/headless builds and tests (the GitHub checks)
```

CAUTION: The `run` and `run-headless` recipes take the preset as the first
argument. Always give the preset first. If you type
`just run-headless --turns 4`, the recipe reads `--turns` as a preset name.

`just ci` uses the compilers in `CC` and `CXX`. If they are not set, it uses
`gcc` and `g++`. To use other compilers, set the variables, for example
`CC=gcc-16 CXX=g++-16 just ci`.

## CMake options

| option | default | effect |
|---|---|---|
| `PIGPEN_BUILD_GUI` | `ON` | Builds `pig-pen`. Set it to `OFF` to build without SDL3, ImGui, and OpenGL. |
| `PIGPEN_BUILD_TESTS` | `ON` | Builds the two Catch2 test binaries and the fake llamad, and registers all CTest cases. |
| `PIGPEN_WARNINGS_AS_ERRORS` | `ON` | Uses `-Werror` for Pig Pen code only. |

The presets set these options explicitly. When you run a preset again, it sets
these options back to the preset values. This also occurs when the cache has
these options set to `OFF`.

Pig Pen uses C++26 reflection only in `pigpen_agent`. All other code builds as
C++23 (refer to [Architecture](architecture.md#build-layout)). But
`pigpen_agent` and scrydantic need GCC 16.2. Because of this, the configure
step does not accept compilers that are not GNU, or GCC versions before 16.2.

Pig Pen builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. These
flags apply only to Pig Pen source files. The build adds the fetched
dependencies and the generated llamad stubs as `SYSTEM`, and they keep their
own warning settings.

To build only the CLI, use the commands below. A GUI toolchain is not
necessary.

```sh
cmake --preset headless
cmake --build --preset headless
```

## Use a local checkout of a dependency

The FetchContent override can point a dependency to a local checkout. A Pig
Pen option is not necessary:

```sh
cmake --preset dev \
  -DFETCHCONTENT_SOURCE_DIR_SCRYDANTIC=/absolute/path/to/scrydantic \
  -DFETCHCONTENT_SOURCE_DIR_LLAMAD=/absolute/path/to/llamad
```

Use an absolute path. If the path is relative, CMake gives a warning, because
relative `FETCHCONTENT_SOURCE_DIR_*` values depend on the calling scope. If
the scrydantic checkout does not supply the `scrydantic::scrydantic` target,
or the llamad checkout has no `proto/llamad/v1/llamad.proto`, the configure
step fails. The preset CMake cache keeps the override. To use the pinned
versions again, delete `build/<preset>/`.

## Troubleshooting

**The configure step does not finish, or it fails on the first run.** The
configure step clones the dependencies. Examine the network access and the
proxy settings. The configure log shows the `GIT_PROGRESS` output.

**The configure step cannot find gRPC or Protobuf.** Install the development
packages, with `protoc` and `grpc_cpp_plugin`. CMake first looks for the
Protobuf CMake config, and then uses its own `FindProtobuf` module. gRPC must
supply its CMake config (`gRPCConfig.cmake`).

**SDL3 cannot enable a Linux display backend.**

1. Install the X11 development headers, the Wayland development headers, or
   both. The package lists above include them.
2. Delete `build/dev/`.
3. Do the configure step again, so that SDL3 does its feature detection again.

**A dependency is old after you use a local source override or change a
pinned tag.** `FetchContent` keeps a cache in `build/<preset>/_deps`. Remove
that directory or the full build directory. Then do the configure step again.

**CMake shows an older compiler after you install GCC 16.2.** CMake keeps the
compiler selection in the cache of each build directory. Configure again in a
new directory. On systems that install compiler executables with version
names, you can also run
`cmake --preset dev --fresh -DCMAKE_CXX_COMPILER=g++-16`.
