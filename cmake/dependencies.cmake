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

# gRPC and Protobuf come from the system. Prefer Protobuf's own CMake config
# and fall back to CMake's FindProtobuf module, as llamad's build does.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)

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
