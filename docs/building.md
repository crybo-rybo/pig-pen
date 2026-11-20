# Build Pig Pen

## Prerequisites

| requirement | notes |
|---|---|
| CMake 3.31 or later | The presets use schema version 6. |
| Ninja | The presets use the Ninja generator. |
| GCC 16.2 or later | Necessary for C++26 P2996/P3394 reflection. scrydantic refuses older compilers, also GCC 16 snapshots before 16.2. |
| gRPC and Protobuf for C++ | With `protoc` and `grpc_cpp_plugin`. The build generates the llamad stubs with them. They must be built with the same C++ standard library as Pig Pen (libstdc++). On Linux, the system packages are applicable. On macOS, `scripts/build-grpc.sh` builds them (refer to [macOS](#macos)). |
| Python 3 | Necessary when the test suite is on. Python runs the CLI tests. |
| OpenGL 3.2 or later | Necessary only for the GUI target. SDL3 creates the platform context. |

The configure step gets all other dependencies at pinned versions.
`cmake/dependencies.cmake` and `cmake/testing.cmake` specify these
dependencies:

- [scrydantic](https://github.com/crybo-rybo/scrydantic) 0.2.0 (the commit
  of the tag `v0.2.0`)
- the `llamad.proto` file of [llamad](https://github.com/crybo-rybo/llamad)
  0.2.0. The pin is the commit of `main` that sets the version to 0.2.0,
  because 0.2.0 does not have a tag yet. The build does not clone the
  llama.cpp submodule, and does not build llamad.
- SDL3 and Dear ImGui, for the GUI
- Catch2 and nlohmann/json, for the tests

The first configure step clones them. Network access is necessary for this
step, and it takes some minutes. Later configure steps use the files in
`build/<preset>/_deps` again.

Pig Pen does not build or start llamad. To run an episode, build llamad from
its own repository and start it. Refer to [Run Pig Pen](running.md).

Before version 1.0, a minor release of llamad can change its wire contract.
Thus, the daemon must be of the same minor series as the pinned
`llamad.proto` (0.2.x). Pig Pen refuses another version when it connects.
When you move the llamad pin to another minor version, also change
`required_llamad_series` in `src/llamad/llamad_client.cpp`.

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

If you have a GCC 16.2 and your system gRPC is not applicable, you can use
the same script as on macOS: `CC=gcc-16 CXX=g++-16 scripts/build-grpc.sh`.
On Linux, the script uses the system OpenSSL, zlib, and c-ares. On Debian and
Ubuntu, install `git cmake ninja-build libssl-dev zlib1g-dev libc-ares-dev`.
The developers tested the script only on macOS.

## macOS

On macOS, Pig Pen uses two compilers:

- Homebrew GCC 16 compiles the C++ code. GCC 16.2 or later is necessary.
- Apple Clang compiles the SDL3 C and Objective-C platform code.

Pig Pen must compile with GCC and libstdc++, because Apple Clang has no C++26
reflection. The Homebrew gRPC and Protobuf packages are built with Apple Clang
and libc++. GCC code cannot link them. Because of this, you build gRPC one
time with the same GCC. The script `scripts/build-grpc.sh` does this.

1. Install the packages:

   ```sh
   brew install gcc cmake ninja just openssl@3 c-ares
   g++-16 --version   # must show 16.2 or later
   ```

2. Build gRPC:

   ```sh
   scripts/build-grpc.sh   # or: just grpc
   ```

   The script clones gRPC v1.84.0, and makes sure that the tag is at the
   pinned commit. It builds gRPC with the Protobuf, Abseil, and RE2 versions
   that gRPC bundles. It installs them as static libraries into
   `build/deps/grpc`. Network access is necessary. The build takes about ten
   minutes on 12 cores. The installed prefix is about 120 MB.

   The script writes a stamp file into the prefix. When the stamp shows the
   same gRPC version and the same compilers, the script stops immediately.
   To build again, use `--force`. For all options, run
   `scripts/build-grpc.sh --help`.

3. Configure, build, and test. Select the compilers on the first configure
   step:

   ```sh
   CC=/usr/bin/cc CXX=g++-16 cmake --preset dev --fresh
   cmake --build --preset dev
   ctest --preset dev
   ```

The configure step uses `build/deps/grpc` automatically when it exists. It
searches that prefix before the Homebrew and system directories. Thus, a
Homebrew gRPC can stay installed. To use a gRPC in a different prefix, give
`-DPIGPEN_GRPC_ROOT=/absolute/path/to/prefix`. The configure step shows the
gRPC that it uses, for example
`Using gRPC 1.84.0 from .../build/deps/grpc/lib/cmake/grpc`.

The configure step then links a small test program with that gRPC. If the
link fails, the configure step stops. The message names the gRPC directory.
Usually, that gRPC was built with a different C++ standard library, for
example the Homebrew gRPC. Without this check, the build fails only at the
end, when the executables link (undefined `std::__cxx11` symbols).

The binaries link gRPC, Protobuf, and Abseil statically. They use the
Homebrew OpenSSL and c-ares as shared libraries.

`build/deps/grpc` is in the `build/` directory. If you delete `build/`, you
also delete the gRPC build. To keep it, delete only `build/<preset>/`.

SDL3 keeps Cocoa behind its C API. Because of this, GCC does not parse the
Apple framework headers when it compiles the ImGui backend and the
application. Pig Pen still uses the deprecated Apple OpenGL implementation to
render. The build suppresses that SDK deprecation diagnostic.

The developers tested this setup on macOS 26 on Apple silicon, with Homebrew
GCC 16.2. The hosted CI does not test macOS.

## Presets

```sh
cmake --preset dev            # Debug, GUI + tests, warnings as errors
cmake --build --preset dev
```

The `release` preset is the same as `dev`, but with
`CMAKE_BUILD_TYPE=Release`. The `headless` preset is a Debug build with
`PIGPEN_BUILD_GUI=OFF` (no SDL3, ImGui, or OpenGL). The `asan` preset is the
same as `headless`, but it compiles and links with AddressSanitizer and
UndefinedBehaviorSanitizer (refer to
[Testing](testing.md#run-the-suite-with-the-sanitizers)). Each preset writes to
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
just ci                        # format + lint + dev/release/headless/asan builds and tests (the GitHub checks)
```

CAUTION: The `run` and `run-headless` recipes take the preset as the first
argument. Always give the preset first. If you type
`just run-headless --turns 4`, the recipe reads `--turns` as a preset name.

`just ci` uses the compilers in `CC` and `CXX`. If they are not set, it uses
`gcc` and `g++`. On macOS, it uses `/usr/bin/cc` and `g++-16`. To use other
compilers, set the variables, for example `CC=gcc-16 CXX=g++-16 just ci`.

`just grpc` runs `scripts/build-grpc.sh`. The recipe sends all arguments to
the script, for example `just grpc --force`.

## CMake options

| option | default | effect |
|---|---|---|
| `PIGPEN_BUILD_GUI` | `ON` | Builds `pig-pen`. Set it to `OFF` to build without SDL3, ImGui, and OpenGL. |
| `PIGPEN_BUILD_TESTS` | `ON` | Builds the two Catch2 test binaries and the fake llamad, and registers all CTest cases. |
| `PIGPEN_WARNINGS_AS_ERRORS` | `ON` | Uses `-Werror` for Pig Pen code only. |
| `PIGPEN_GRPC_ROOT` | *(empty)* | A gRPC and Protobuf install prefix. The configure step searches it first. If it is empty and `build/deps/grpc` exists, the configure step uses `build/deps/grpc`. |

The presets set the first three options explicitly. When you run a preset again, it sets
these options back to the preset values. This also occurs when the cache has
these options set to `OFF`.

Pig Pen uses C++26 reflection only in `pigpen_agent` and the reflection
tests. All other code builds as C++23, also `pigpen_runtime` with `Session`
(refer to [Architecture](architecture.md#build-layout)). But
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
packages, with `protoc` and `grpc_cpp_plugin`. On macOS, run
`scripts/build-grpc.sh`. CMake first looks for the Protobuf CMake config, and
then uses its own `FindProtobuf` module. gRPC must supply its CMake config
(`gRPCConfig.cmake`).

**The configure step stops with "Pig Pen cannot link the gRPC at ...".** The
configure step found a gRPC that was built with a different C++ standard
library. On macOS, this is usually the Homebrew gRPC.

1. Run `scripts/build-grpc.sh` (or `just grpc`).
2. Configure again with `--fresh`, for example
   `CC=/usr/bin/cc CXX=g++-16 cmake --preset dev --fresh`.

`CMakeFiles/CMakeConfigureLog.yaml` in the build directory shows the failed
link.

**The configure step uses the old gRPC after you build gRPC or change
`PIGPEN_GRPC_ROOT`.** CMake keeps `gRPC_DIR`, `Protobuf_DIR`, and the
directories of the other packages in the cache. Configure again with
`--fresh`.

**`scripts/build-grpc.sh` stops with "is not GCC 16.2 or later".** The script
uses `gcc-16` and `g++-16` on macOS, and `gcc` and `g++` on other systems.
Set `CC` and `CXX` to the GCC that builds Pig Pen.

**SDL3 cannot enable a Linux display backend.**

1. Install the X11 development headers, the Wayland development headers, or
   both. The package lists above include them.
2. Delete `build/dev/`.
3. Do the configure step again, so that SDL3 does its feature detection again.

**A dependency is old after you use a local source override or change a
pinned commit.** `FetchContent` keeps a cache in `build/<preset>/_deps`. Remove
that directory or the full build directory. Then do the configure step again.

**CMake shows an older compiler after you install GCC 16.2.** CMake keeps the
compiler selection in the cache of each build directory. Configure again in a
new directory. On systems that install compiler executables with version
names, you can also run
`cmake --preset dev --fresh -DCMAKE_CXX_COMPILER=g++-16`.
