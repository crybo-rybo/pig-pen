# All third-party fetching and upstream option pinning lives here so the root
# CMakeLists.txt only describes Pig Pen's own targets. Everything is fetched
# with SYSTEM + EXCLUDE_FROM_ALL: warnings stay off for pinned code and it is
# kept out of the default test/example graph.
include(FetchContent)

# --- scry ---------------------------------------------------------------------

set(SCRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_FORMAT_CHECK OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(SCRY_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(SCRY_ENABLE_REFLECTION ON CACHE BOOL "" FORCE)
set(SCRY_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_IMGUI_SHOWCASE OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_LOCAL_MODEL_SMOKE OFF CACHE BOOL "" FORCE)

# Keep the previously documented project option as a compatibility alias. It
# resolves relative paths against the project source, unlike FetchContent's
# scope-dependent relative-path handling.
if(PIGPEN_SCRY_SOURCE)
  get_filename_component(
    PIGPEN_SCRY_SOURCE_ABSOLUTE
    "${PIGPEN_SCRY_SOURCE}"
    ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(NOT EXISTS "${PIGPEN_SCRY_SOURCE_ABSOLUTE}/CMakeLists.txt")
    message(
      FATAL_ERROR
      "PIGPEN_SCRY_SOURCE does not contain CMakeLists.txt: "
      "${PIGPEN_SCRY_SOURCE_ABSOLUTE}"
    )
  endif()

  if(FETCHCONTENT_SOURCE_DIR_SCRY)
    get_filename_component(
      FETCHCONTENT_SOURCE_DIR_SCRY_ABSOLUTE
      "${FETCHCONTENT_SOURCE_DIR_SCRY}"
      ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(NOT PIGPEN_SCRY_SOURCE_ABSOLUTE STREQUAL
       FETCHCONTENT_SOURCE_DIR_SCRY_ABSOLUTE)
      message(
        FATAL_ERROR
        "PIGPEN_SCRY_SOURCE and FETCHCONTENT_SOURCE_DIR_SCRY select different checkouts"
      )
    endif()
  endif()

  message(
    DEPRECATION
    "PIGPEN_SCRY_SOURCE is deprecated; use FETCHCONTENT_SOURCE_DIR_SCRY"
  )
  set(
    FETCHCONTENT_SOURCE_DIR_SCRY
    "${PIGPEN_SCRY_SOURCE_ABSOLUTE}"
    CACHE PATH
    "Use a local scry checkout instead of fetching the pinned revision"
    FORCE
  )
endif()

# To build against a local scry checkout instead of the pinned revision, pass
# CMake's built-in override with an absolute path:
#   cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_SCRY=/path/to/scry
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.2.0
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

# --- nlohmann/json ------------------------------------------------------------

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(scry nlohmann_json)

if(NOT TARGET scry::reflection)
  message(FATAL_ERROR "The selected Scry source does not provide scry::reflection")
endif()

# --- SDL3 + Dear ImGui (GUI only) ---------------------------------------------

if(PIGPEN_BUILD_GUI)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # SDL 3.4.14. Pin the commit rather than a mutable branch.
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
