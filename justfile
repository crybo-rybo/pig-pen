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

# Same pinned versions as .github/workflows/ci.yml.
clang_format := "uvx clang-format@22.1.8"
ruff := "uvx ruff@0.16.1"

# macOS needs Apple Clang for SDL3's Objective-C (GCC lacks -fobjc-arc).
ci_cc := if os() == "macos" { "/usr/bin/cc" } else { "gcc-16" }

fmt:
    git ls-files '*.cpp' '*.hpp' | xargs {{clang_format}} -i
    {{ruff}} format tests

fmt-check:
    git ls-files '*.cpp' '*.hpp' | xargs {{clang_format}} --dry-run --Werror
    {{ruff}} format --check tests

lint:
    {{ruff}} check tests

# Everything GitHub runs on a PR. Reuses fetched deps in build/<preset>/.
ci: fmt-check lint (ci-build-test "dev") (ci-build-test "release") (ci-build-test "headless")

[private]
ci-build-test profile:
    CC={{ci_cc}} CXX=g++-16 cmake --preset {{profile}} --fresh
    cmake --build --preset {{profile}}
    ctest --preset {{profile}}
