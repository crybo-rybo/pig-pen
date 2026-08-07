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
