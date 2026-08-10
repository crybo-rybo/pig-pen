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

fmt:
    git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
    {{ruff}} format tests

fmt-check:
    git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror
    {{ruff}} format --check tests

lint:
    {{ruff}} check tests
