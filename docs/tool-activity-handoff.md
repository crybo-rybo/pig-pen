# Tool activity refactor handoff

Status: follow-up work after the initial C++26 reflection boundary lands. This
is not required to merge that change.

## Motivation

Scry now owns provider-facing schema generation, strict argument decoding, and
typed response encoding. Pig Pen still projects the same typed callback values
back into JSON through `reflected_json.hpp` so `WorldEvent`, the UI, and the
JSONL writer can inspect them. The UI then parses JSON keys to recover domain
facts that were already known by the typed callback.

The follow-up should keep JSON at external boundaries rather than in Pig Pen's
application model:

```text
model <-> Scry JSON/reflection <-> typed Pig Pen callbacks
                                      |
                                      v
                              typed tool activity
                               |             |
                               v             v
                              UI        metrics output
```

## Proposed design

- Add a `WorldToolController` application service. Its reflected `move`,
  `look`, and `eat` handlers should invoke `WorldTools`, publish one activity,
  and return only the typed response to Scry.
- Add a `ToolActivityJournal` owned by `Session`. It should append activities,
  maintain call/eaten counters, expose read-only state to both front ends, and
  forward records to `MetricsWriter`.
- Replace JSON-bearing `WorldEvent` with a small typed `ToolActivity`: tool
  kind, outcome, turn/tick, before/after positions, optional direction/eaten
  item, and a presentation summary if useful.
- Make the UI, animation, headless output, and stats consume typed fields. They
  should not parse argument or result JSON.
- Keep serialization inside `MetricsWriter` if JSONL remains desirable, or
  adopt a simpler text/TSV format. The persistent record should describe Pig
  Pen semantics rather than mirror Scry's exact provider payload.

Do not replace this with Scry's `on_tool_call` observer: that observer carries
protocol metadata and raw JSON arguments, but not the typed response or Pig
Pen's world transition. The reflected handler wrapper is the point that has all
required typed information.

## Migration outline

1. Introduce `ToolKind`, `ToolOutcome`, `ToolActivity`, and
   `ToolActivityJournal` with focused unit tests.
2. Move per-call orchestration out of `Session::register_tools` into
   `WorldToolController`; leave registration as metadata plus thin typed
   callables.
3. Convert animation, UI panels, headless printing, and statistics to the
   journal.
4. Simplify the metrics contract and update its tests and documentation.
5. Delete `reflected_json.hpp`, remove JSON from the event/application layer,
   and stop asserting that Pig Pen logs exactly match Scry's encoded result.

## Invariants to preserve

- Unknown or schema-invalid calls remain Scry-owned and never reach Pig Pen's
  world, activity journal, or four-action application budget.
- A decoded call rejected by Pig Pen's action budget still creates one activity
  with no world mutation.
- Tool execution and activity publication remain synchronous on the thread
  calling `Session::pump`; the UI continues to poll read-only state rather than
  being called from a tool handler.
- The committed typed tool response must reach Scry even if metrics persistence
  fails; the episode may fail immediately after dispatch returns.
- GUI and headless modes continue to share the same controller, journal, and
  metrics behavior.

## Completion criteria

- `reflected_json.hpp` is gone.
- `Session`, tool activity, animation, and UI code do not depend on
  `nlohmann::json`.
- Scry remains the only model-facing schema/decode/encode implementation.
- Tool counters, animation, summaries, and metrics reconcile from the same
  journal record.
- Public-boundary tests still cover valid reflected calls, invalid argument
  rejection, world mutation, and typed responses returned to the model.
