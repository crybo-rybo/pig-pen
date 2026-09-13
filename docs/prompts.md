# Prompts and compiled resource reference

Edit [`resources/prompts.json`](../resources/prompts.json) for system and turn
instructions, visibility variants, zero-tool recovery, the human-guidance
heading, tool descriptions (including the direction annotation), and tool-budget
messages. The long headless help text lives in
[`resources/cli.json`](../resources/cli.json).

Rebuild normally after editing:

```sh
cmake --build --preset headless
# Or: just build dev
```

GCC's `#embed` dependency tracking rebuilds the consumers automatically. The
executables contain the decoded text; they never read these JSON files at
runtime, and changing a JSON file without rebuilding does not change a running
or previously built executable.

## Catalog format

Each file is a UTF-8 JSON object. A value is either a string or an array of
strings. Array fragments are joined **verbatim without separators**. Keep a
trailing space between prose fragments where a word boundary is needed; use
`\n` for a newline. Arrays only wrap long text for easier editing.

```json
{
  "example": [
    "First sentence. ",
    "Second sentence.\n"
  ]
}
```

The reader accepts JSON string escapes, including Unicode escapes and surrogate
pairs. Invalid JSON, invalid UTF-8, duplicate keys, unsupported value types, and
missing referenced keys fail constant evaluation and stop compilation. Objects
inside objects, numbers, booleans, and null are intentionally outside this text
catalog format. No additional JSON library or build-time script is required.

Formatted entries use C++ `std::format` fields. `{}` consumes the next argument;
`{0}`, `{1}`, etc. let you reorder them. In these entries, write `{{` and `}}`
for literal braces. C++ checks field syntax, indices, and argument types at
compile time. Plain entries are copied literally. Human guidance is appended
literally and is never interpreted as a format string.

| Entry | Arguments, in order |
|---|---|
| `system_coordinates` | spawn x, spawn y |
| `system_limits` | turn budget, tool rounds per turn, world-tool calls per turn |
| `turn_instructions` | world-tool calls per turn, current turn, turn budget |
| `budget_one_remaining`, `budget_many_remaining` | remaining calls |
| `headless_usage` | executable name |

Existing keys are wired to named constants in `src/agent/prompt_text.hpp`.
Adding a new behavior or conditional section still requires a C++ call site;
changing its wording requires only the JSON edit. Scenario selection and numeric
limits remain determined by the episode configuration.

## Build manifest

Normal builds, including explicit builds of either application target, also
write:

```text
build/<preset>/resources/build-manifest.json
```

This deterministic reference contains:

- Build configuration, project version, compiler version, and C++ language level.
- All decoded prompt templates and CLI text.
- Tool names and descriptions, plus reflected input and result JSON Schemas.
- Reflected default configuration and the world-tool call limit.
- Rendered system prompts for all eight visibility-flag combinations, each with
  the configuration used to render it, and automatic/recovery turn examples.

C++26 reflection generates the schemas from the same argument/result types used
by Scry. `tool_definitions.hpp` enumerates the bindings once for both live
registration and export. Prompt examples call the application's actual builders.
The templates and schemas are compiled into `pigpen-resource-manifest`; running
that helper during the build writes the JSON. Reflection and constant evaluation
do not themselves write files.

Examples use default budgets, not settings from a particular episode. The
artifact contains all visibility variants, including the known reward table;
it is a developer reference, not a file to send wholesale to the model. Archive
it alongside a binary when preserving an experiment's build. It is a content
reference, not a cryptographic binary identity or a log of runtime human input.

Generate just this artifact or export the compiled helper's data elsewhere:

```sh
cmake --build build/headless --target pigpen_resources
./build/headless/pigpen-resource-manifest /tmp/build-manifest.json
```

The exporter needs no model, credentials, source JSON files, or network. If an
artifact is deleted, the next build recreates it. Tool/type/prompt changes rebuild
the helper and refresh the artifact. Existing build/test presets are native
builds; executing this helper when cross-compiling requires a target emulator.
