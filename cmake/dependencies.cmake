# Third-party dependencies, pinned. Fetched sources are SYSTEM (no warnings
# from pinned code) and EXCLUDE_FROM_ALL (only what Pig Pen links is built).
# Upstream tests, examples, and install rules already default to off when a
# project is not top-level, so only non-default choices are set here.
#
# To build against a local checkout instead of a pinned revision, use the
# standard FetchContent override with an absolute path:
#   cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_SCRYDANTIC=/abs/path/scrydantic
#   cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_LLAMAD=/abs/path/llamad
include(FetchContent)

# scrydantic: C++26 reflection to JSON Schema, a strict codec, and tool
# binding. Its CMake refuses compilers older than GCC 16.2.
FetchContent_Declare(
  scrydantic
  GIT_REPOSITORY https://github.com/crybo-rybo/scrydantic.git
  GIT_TAG v0.2.0
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

# llamad: only its wire contract (proto/llamad/v1/llamad.proto) is used. No
# submodules (llama.cpp) are cloned and its CMake project is never added.
FetchContent_Declare(
  llamad
  GIT_REPOSITORY https://github.com/crybo-rybo/llamad.git
  GIT_TAG v0.1.0
  GIT_SUBMODULES ""
  GIT_PROGRESS TRUE
  SOURCE_SUBDIR cmake-not-used
)

FetchContent_MakeAvailable(scrydantic llamad)

if(NOT TARGET scrydantic::scrydantic)
  message(
    FATAL_ERROR
    "The selected scrydantic source does not provide scrydantic::scrydantic"
  )
endif()
set(PIGPEN_LLAMAD_PROTO "${llamad_SOURCE_DIR}/proto/llamad/v1/llamad.proto")
if(NOT EXISTS "${PIGPEN_LLAMAD_PROTO}")
  message(
    FATAL_ERROR
    "The selected llamad source has no proto/llamad/v1/llamad.proto"
  )
endif()

# gRPC and Protobuf come from the system, or from a prefix that
# scripts/build-grpc.sh built with the project compiler. That script installs
# into build/deps/grpc by default, and that prefix is used when it exists. The
# prefix is searched first. To change it in an existing build directory,
# configure again with --fresh.
set(
  PIGPEN_GRPC_ROOT ""
  CACHE PATH "A gRPC and Protobuf install prefix to search first"
)
set(_pigpen_grpc_root "${PIGPEN_GRPC_ROOT}")
if(_pigpen_grpc_root STREQUAL "" AND
   EXISTS "${PROJECT_SOURCE_DIR}/build/deps/grpc/lib/cmake/grpc")
  set(_pigpen_grpc_root "${PROJECT_SOURCE_DIR}/build/deps/grpc")
endif()
if(NOT _pigpen_grpc_root STREQUAL "")
  if(NOT IS_DIRECTORY "${_pigpen_grpc_root}")
    message(
      FATAL_ERROR
      "PIGPEN_GRPC_ROOT is not a directory: ${_pigpen_grpc_root}"
    )
  endif()
  list(PREPEND CMAKE_PREFIX_PATH "${_pigpen_grpc_root}")
endif()

# Prefer Protobuf's own CMake config and fall back to CMake's FindProtobuf
# module, as llamad's build does.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION} from ${gRPC_DIR}")

# GCC code cannot link a gRPC that was built for a different C++ standard
# library, for example Homebrew's (Apple Clang and libc++). Without this check,
# that fails only when the executables link. A passing result is cached for
# each gRPC_DIR; a failing result is never cached.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
if(NOT "${PIGPEN_GRPC_LINK_CHECKED_DIR}" STREQUAL "${gRPC_DIR}")
  unset(PIGPEN_GRPC_LINKS CACHE)
endif()
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES gRPC::grpc++)
check_cxx_source_compiles(
  [=[
    #include <string>
    #include <grpcpp/grpcpp.h>
    int main() {
      const std::string target = "unix:/tmp/pigpen-link-check.sock";
      auto channel =
          grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
      return channel ? 0 : 1;
    }
  ]=]
  PIGPEN_GRPC_LINKS
)
cmake_pop_check_state()
if(NOT PIGPEN_GRPC_LINKS)
  unset(PIGPEN_GRPC_LINKS CACHE)
  message(
    FATAL_ERROR
    "Pig Pen cannot link the gRPC at ${gRPC_DIR}. That gRPC was built with a "
    "different C++ standard library than this compiler uses "
    "(${CMAKE_CXX_COMPILER}). Run scripts/build-grpc.sh (or just grpc) to "
    "build gRPC with this compiler, then configure again with --fresh. See "
    "docs/building.md. CMakeFiles/CMakeConfigureLog.yaml shows the failed "
    "link."
  )
endif()
set(PIGPEN_GRPC_LINK_CHECKED_DIR "${gRPC_DIR}" CACHE INTERNAL "")

if(PIGPEN_BUILD_GUI)
  # Link SDL3 statically into the GUI binary.
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # SDL 3.4.14
    GIT_TAG 147a8ee32dbf9ac02f3794964490687b6bbda1bc
    GIT_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG e0b59689461f03cbdc9aa394ea1512c1f0e6e246
    GIT_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(SDL3 imgui)
endif()
