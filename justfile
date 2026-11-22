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

# Build the pinned gRPC with the project compiler (scripts/build-grpc.sh --help).
grpc *args:
    scripts/build-grpc.sh {{args}}

# Same pinned versions as .github/workflows/ci.yml.
clang_format := "uvx clang-format@22.1.8"
ruff := "uvx ruff@0.16.1"

# The compilers for `ci`: CC and CXX when set. Else gcc and g++ (GCC 16.2 or
# later, as in the gcc:16.2.0 CI image). On macOS, Apple Clang compiles SDL3's
# Objective-C (GCC lacks -fobjc-arc) and Homebrew's g++-16 compiles the C++.
ci_cc := env("CC", if os() == "macos" { "/usr/bin/cc" } else { "gcc" })
ci_cxx := env("CXX", if os() == "macos" { "g++-16" } else { "g++" })

fmt:
    git ls-files '*.cpp' '*.hpp' | xargs {{clang_format}} -i
    {{ruff}} format tests

fmt-check:
    git ls-files '*.cpp' '*.hpp' | xargs {{clang_format}} --dry-run --Werror
    {{ruff}} format --check tests

lint:
    {{ruff}} check tests

# Everything GitHub runs on a PR. Reuses fetched deps in build/<preset>/.
ci: fmt-check lint (ci-build-test "dev") (ci-build-test "release") (ci-build-test "headless") (ci-build-test "asan")

[private]
ci-build-test profile:
    CC={{ci_cc}} CXX={{ci_cxx}} cmake --preset {{profile}} --fresh
    cmake --build --preset {{profile}}
    ctest --preset {{profile}}
