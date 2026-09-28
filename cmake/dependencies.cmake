# Third-party dependencies, pinned. Everything is fetched SYSTEM (no warnings
# from pinned code) and EXCLUDE_FROM_ALL (only what pig-pen links is built).
# Upstream tests, examples, and install rules already default to off when a
# project is not top-level, so only non-default choices are set here.
#
# To build against a local scry checkout instead of the pinned revision:
#   cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_SCRY=/absolute/path/to/scry
include(FetchContent)

# scry::testing (the scripted transport) is only needed by the test suite.
set(SCRY_BUILD_TESTING_SUPPORT ${PIGPEN_BUILD_TESTS} CACHE BOOL "" FORCE)

# Deprecated compatibility alias for FETCHCONTENT_SOURCE_DIR_SCRY. It resolves
# relative paths against the project source, unlike FetchContent's
# scope-dependent relative-path handling, and fails rather than silently
# building the pinned revision when the checkout is invalid or conflicts.
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

FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.5.0
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b
  GIT_PROGRESS TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(scry nlohmann_json)

if(NOT TARGET scry::scry)
  message(FATAL_ERROR "The selected Scry source does not provide scry::scry")
endif()

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
