set shell := ["bash", "-euo", "pipefail", "-c"]

default: build

configure profile="dev":
    cmake --preset {{profile}}

build profile="dev": (configure profile)
    cmake --build --preset {{profile}}

test profile="dev": (build profile)
    ctest --preset {{profile}}

run profile="dev" *args: (configure profile)
    cmake --build --preset {{profile}} --target pig-pen
    ./build/{{profile}}/pig-pen {{args}}

run-headless profile="dev" *args: (configure profile)
    cmake --build --preset {{profile}} --target pig-pen-headless
    ./build/{{profile}}/pig-pen-headless {{args}}

# Pin the same versions CI uses (.github/workflows/format.yml) so local
# formatting and the format check agree byte-for-byte.
ruff := "uvx ruff@0.16.1"

# Pin compilers with -D so an existing CMake cache is updated too. On
# macOS the C compiler must be Apple Clang: SDL3 requires -fobjc-arc,
# which GCC does not implement.
ci_cmake_compiler := if os() == "macos" {
    "-DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=g++-16"
} else {
    "-DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16"
}

fmt:
    git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
    {{ruff}} format tests

fmt-check:
    git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror
    {{ruff}} format --check tests

lint:
    {{ruff}} check tests

# Everything GitHub runs on a PR: format.yml (fmt-check + lint) plus the
# ci.yml matrix (dev, release, headless). Reuses build/<preset>/ if present.
ci: fmt-check lint (ci-build-test "dev") (ci-build-test "release") (ci-build-test "headless")

# One CI matrix job: configure, build, and test a named preset with the
# compilers GitHub uses. CMake aborts the same invocation when
# CMAKE_C_COMPILER changes in an existing cache ("variables that require
# your cache to be deleted"); retry once against the cleared cache.
[private]
ci-build-test profile:
    cmake --preset {{profile}} {{ci_cmake_compiler}} \
        || cmake --preset {{profile}} {{ci_cmake_compiler}}
    cmake --build --preset {{profile}}
    ctest --preset {{profile}} --output-on-failure
