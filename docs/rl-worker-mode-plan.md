# Plan: RL worker mode

Status: phases 0 (episode facts), 1 (reward and summary), and 2 (the seams)
are implemented; later phases are proposals.

Pig Pen today runs one episode at a time under a front end that owns the pump
loop (the GUI once per frame, `pig-pen-headless` in a sleep-1 ms loop) and
writes one JSONL log per episode. This plan adds a second execution mode, an
**RL worker**, that runs batches of episodes against a trainer's inference
server and emits one machine-readable reward record per episode. It also adds
a shaped reward that penalises rejected and invalid tool calls and rewards
exploration and action, in both modes.

The guiding constraint: **the episode must not know which mode it is in.** The
world, prompts, tool contract, call budget, round limit, and the runner are
byte-for-byte the same in evaluation and training, so a policy trains on
exactly the harness it is later evaluated on. The two modes differ only in who
owns the pump loop, how many sessions it pumps, and what it does with the
facts an episode leaves behind.

## 1. Where the seam falls

### 1.1 What is already mode-agnostic

| unit | reuse status |
|---|---|
| `world::World` | unchanged; seed-determinism is exactly what group-based RL (GRPO-style N seeds × K samples) needs |
| `WorldTools`, `WorldToolBinding`, `tool_contract.hpp` | unchanged; the model-facing contract is shared by construction |
| `prompt.cpp` | unchanged; the training prompt is the evaluation prompt |
| `EpisodeRunner` + `ITurnTransport` | unchanged; guidance and pause are simply unused by the worker |
| `ScryTurnTransport`, `scry_config()` | gains request headers (rollout identity); otherwise unchanged |
| `Session` | stays the composition root and reset unit; becomes the single source of episode *facts* |

### 1.2 What is currently welded to the interactive mode, and how it comes apart

| today | problem for a worker | change |
|---|---|---|
| `Session::create` always opens a JSONL `MetricsWriter` | a training worker running thousands of episodes should not write a file per episode by default | JSONL becomes an optional `SessionOptions::log_directory` |
| `Session::create` reads `PIGPEN_API_KEY` via `getenv` | hidden environment dependency inside the runtime layer | moves to the front ends; `SessionOptions::api_key` |
| `MetricsWriter` accumulates footer facts (tool counts, items eaten, last score, duration) incrementally | the worker needs the same facts without a file | facts live on `Session` (world, activity feed, retained `TurnRecord`s, start time); a pure `summarize_episode()` builds an `EpisodeSummary` that both the footer and the worker serialise |
| `headless_main.cpp` inlines the drive-to-completion loop (deadline, cooperative cancel, 15 s grace) | the worker needs the identical loop for P sessions at once | extracted into `agent::EpisodeDriver` over a minimal `IDrivableEpisode` interface |
| `headless_main.cpp` and `ui/gui_options.cpp` each parse `Config` flags | a third front end would be a third copy | shared `pigpen_cli` library: an option-table parser plus `add_config_options()` |
| there is no notion of "which rollout is this request for" | the trainer's inference server must pair its sampled tokens with Pig Pen's reward | `SessionOptions::rollout_id` → `X-Pigpen-Rollout` request header via scry `Config::extra_headers`; also recorded in the log header and the summary |

### 1.3 The resulting layering

```
src/app/main.cpp          src/app/headless_main.cpp        src/app/worker_main.cpp
  GUI frame loop            eval CLI: one episode,           RL worker: N seeds × K samples,
                            streamed transcript, JSONL       P in flight, JSONL reward records
      │                          │                                  │
   src/ui/                   src/cli/  (pigpen_cli: option parser, termination signal)
      │                          │                                  │
      └──────────────► agent::Session  ◄────── agent::EpisodeDriver ◄──── agent::EpisodeBatch
                             │                  (one episode, cooperative)   (P drivers, job queue)
                             ├── EpisodeRunner / ScryTurnTransport / WorldToolBinding / World  (unchanged)
                             ├── summarize_episode()  →  EpisodeSummary  ──►  MetricsWriter footer
                             └── compute_reward()     →  RewardBreakdown ──►  worker record
```

Two vocabulary rules keep the seam honest:

- **`Config`** is the episode contract: everything that changes what the model
  sees or how the world behaves, and everything the log header records
  (endpoint, model, seed, budgets, sampling, the three visibility flags). It
  gains nothing mode-specific.
- **`SessionOptions`** is host plumbing: optional log directory,
  `prompt_variant` label, API key, rollout id, extra request headers. A mode
  is expressed entirely through `SessionOptions` and through which front end
  owns the loop. There is deliberately no `mode` enum anywhere in `agent/`.

## 2. Reward design

### 2.1 Principles

- **Score stays the truth.** `World::score()` and the footer's `final_score`
  are untouched. The shaped reward is a separate, versioned function computed
  from recorded facts; it never feeds back into the world or the prompt.
- **Emit counts, not just the total.** Every record carries the raw term
  counts and the weights used, so a trainer can re-weight offline without
  re-running rollouts.
- **Pure and testable.** `compute_reward()` depends on plain structs
  (`World` facts, `ToolActivity`, `TurnRecord`, the finish reason), so it is
  unit-tested without scry, a session, or a network.
- **Hard to game.** Every positive term is bounded by something the model
  cannot inflate: cells observed (≤ 99), turns with an action (≤ turn budget),
  score (≤ 25). The model cannot end an episode early, so "stop acting once
  the score is good" is already penalised by the zero-tool-turn term.

### 2.2 Facts the reward needs (and where each comes from)

| fact | source | new? |
|---|---|---|
| `score` | `World::score()` | no |
| `observed_cells` | `World::observed_count()` (bitset popcount) | **new**, trivial |
| `objective_complete` | `FinishReason::objective_complete` | no |
| `turns_used`, `turn_budget` | `EpisodeSnapshot` | no |
| `executed_actions`, `failed_actions` (wall bump, empty eat) | `ToolActivityFeed` (`ToolOutcome`) | no |
| `active_turns`, `zero_tool_turns` | retained `TurnRecord::tool_calls` per turn | Session must retain turns |
| `invalid_calls` (unknown tool or undecodable arguments) | per turn: `tool_stats.calls − tool_stats.rejected_calls − tool_calls` | derived |
| `budget_refused_calls` (over the four-call limit) | per turn: `tool_stats.rejected_calls − host_refused_calls` | needs the next row |
| `host_refused_calls` (refused by `WorldToolBinding::admit` after objective completion or a log failure) | binding counts its own refusals per turn | **new** counter |

These are folded into one struct attached to each retained turn:

```cpp
/// @brief Where each of a turn's tool requests ended up. All four sum to
/// TurnToolStats::calls. Absent when scry produced no Completion.
struct TurnCallTally {
  std::uint32_t executed{};        // decoded, admitted, ran a world action
  std::uint32_t invalid{};         // admitted, but unknown tool or bad arguments
  std::uint32_t budget_refused{};  // past max_world_tool_calls_per_turn
  std::uint32_t host_refused{};    // refused by admit(): objective done / log failed
};
```

`host_refused` is *not* penalised: it only happens when the model batched more
calls into the same round as the final successful `eat`, which is reasonable
behaviour.

### 2.3 The function

```cpp
struct RewardWeights {
  double score{1.0};             // per point of truthful world score
  double explored_cell{0.05};    // per cell observed beyond the spawn (≤ 99)
  double active_turn{0.1};       // per completed turn with ≥ 1 executed action
  double zero_tool_turn{-1.0};   // per completed turn with no executed action
  double failed_action{-0.1};    // per wall bump or empty eat
  double invalid_call{-0.5};     // per unknown-tool / undecodable request
  double budget_refused_call{-0.25};
  double objective{5.0};         // once, when every positive item is eaten
  double unused_turn{0.1};       // per turn left, only when the objective is met
};

struct RewardBreakdown {
  bool valid{};                  // false → reward is absent; see §2.4
  std::string invalid_reason{};
  double total{};
  // Raw counts, always present so weights can be re-applied offline.
  int score{};
  std::uint32_t explored_cells{}, active_turns{}, zero_tool_turns{},
      failed_actions{}, invalid_calls{}, budget_refused_calls{},
      unused_turns{};
  bool objective_complete{};
  // Per-term contributions (count × weight) for readability in logs.
  std::map<std::string, double> terms{};
};

[[nodiscard]] RewardBreakdown compute_reward(const EpisodeFacts &facts,
                                             const RewardWeights &weights);
```

Defaults are sized so the truthful score dominates: the maximum score is 25,
full exploration is worth about 5, and the activity term at most 2 over a
20-turn budget. Everything is overridable with repeatable `--reward NAME=VALUE`
flags on both CLIs, and the weights actually used are written into every
footer and worker record (`reward_version: 1`, `reward_weights: {...}`).

### 2.4 Validity

A reward is only meaningful for an episode that ended on its own terms.
`valid` is true only for `turn_budget` and `objective_complete`. Episodes that
ended in `error`, `cancelled`, `stopped`, or a worker timeout carry
`valid: false` and an `invalid_reason`, and the trainer is expected to drop
them from the batch rather than treat them as zero reward.

## 3. Worker mode

### 3.1 Why a rollout worker rather than a `reset()/step()` API

An RL trainer needs the exact tokens the policy sampled and their
log-probabilities. Those live in the trainer's own inference server, not in
Pig Pen, and Scry talks to that server over the OpenAI-compatible protocol Pig
Pen already uses. So the worker keeps Pig Pen's role unchanged, *play episodes
and report rewards*, and hands the trainer what it needs to join the two
sides: a rollout id on every request. A Gym-style step API would instead
require re-implementing or exposing Scry's admission, call budget, and round
limit outside the harness, which is exactly the drift between training and
evaluation this design exists to prevent.

### 3.2 Interface

```
pig-pen-worker --model NAME --base-url URL --seeds 1000-1999 --samples 4 \
               [--parallel 8] [--rollout-prefix run42] \
               [--temperature 0.8] [--sampling-seed-base 7] \
               [--turns 20 --max-tool-rounds 8 --hidden-values ...]   # all Config flags
               [--reward invalid_call=-1.0 ...] [--timeout-seconds 300] \
               [--log-dir DIR]
```

- **Jobs** are the cross product of `--seeds` (a range `A-B` or a
  comma-separated list, repeatable) and `--samples K`. Each job gets
  `rollout_id = "<prefix>/<seed>/<sample>"`. With `--sampling-seed-base B`,
  sample *k* is sent with provider seed `B + k`; without it the sampling seed
  is unset (server-random), which is the normal case for training.
- **Every request** carries `X-Pigpen-Rollout: <rollout_id>` (and
  `X-Pigpen-Seed: <seed>`) via `scry::Config::extra_headers`. The trainer's
  server groups the requests of one episode by that header.
- **Output** is JSONL on stdout, one `episode` record per finished job in
  completion order, then one `batch` record; diagnostics go to stderr. The
  `episode` record is the same `EpisodeSummary` the log footer serialises,
  plus `rollout_id`, the full `Config`, and the reward breakdown:

  ```json
  {"type":"episode","rollout_id":"run42/1003/2","seed":1003,"sampling_seed":null,
   "finish_reason":"turn_budget","complete":true,"turns_used":20,"final_score":14,
   "items_eaten":{"apple":2,"berry":5,"toadstool":0,"truffle":0},
   "tool_call_counts":{"eat":7,"look":21,"move":40},
   "calls":{"executed":68,"invalid":2,"budget_refused":1,"host_refused":0},
   "reward":{"valid":true,"total":17.85,"score":14,"explored_cells":57,
             "active_turns":19,"zero_tool_turns":1,"failed_actions":3,
             "invalid_calls":2,"budget_refused_calls":1,"objective_complete":false,
             "unused_turns":0,"terms":{"score":14.0,"explored_cell":2.85,...}},
   "reward_version":1,"reward_weights":{...},"duration_ms":41320,"config":{...}}
  ```

- **Per-episode JSONL logs** are off by default in the worker and enabled with
  `--log-dir`; they are the same files `pig-pen-headless` writes.
- **Exit codes**: 0 every episode valid; 6 batch completed but at least one
  episode invalid; 1 a session could not be created (configuration problem,
  batch aborted); 2 usage; 130/143 signal, after cancelling in-flight episodes
  cooperatively and emitting their records as invalid.

### 3.3 Concurrency model

`Session::pump()` never blocks, and each `scry::Harness` owns its own I/O
thread. So `--parallel P` is one host thread pumping up to P sessions
round-robin, with no locking and no change to the invariant that application
callbacks and tool handlers run on the pump thread. When a driver reports its
episode finished, the batch emits the record, destroys the session (which
finalises any log), and starts the next job. When no session made progress in
a pass, the loop sleeps 1 ms, exactly as the headless CLI does today.

Sessions are created per job. Harness creation starts a worker thread and
initialises libcurl, which is milliseconds against episodes that take seconds
of inference; reusing a harness across episodes would also mean rebinding the
tool registry's captured world, so it is not worth doing until profiling says
otherwise (§6).

### 3.4 Train/eval discipline

The worker does nothing to stop a user from training and evaluating on the
same seeds; that is a trainer-side policy. `docs/training.md` will state the
convention explicitly: pick disjoint seed ranges, and keep the scenario flags
(`--hidden-values`, `--no-reward-feedback`, `--opaque-look`) as curricula or
held-out conditions rather than mixing them silently.

## 4. Work breakdown

Each phase is one pull request, lands with CI green, and, until phase 3,
changes no existing CLI contract (the headless integration test's exact
stdout and JSONL assertions stay untouched).

### Phase 0: episode facts

Goal: `Session` becomes the single source of everything a summary or reward
needs. No new behaviour visible to users except one additive log field (`calls` on
turn records).

- `World::observed_count()`.
- `WorldToolBinding`: count `admit()` refusals per turn; expose and reset
  through the existing `flush_pending_activity()` / turn-completion path.
- `Session` retains `std::vector<EpisodeTurn>` (`TurnRecord` + optional
  `TurnCallTally`) and its creation time; `Session::turns()`,
  `Session::elapsed()`.
- `MetricsWriter::record_turn` writes `"calls": {executed, invalid,
  budget_refused, host_refused}` (or `null`).
- Tests: `world_tests` (observed count), `scry_transport_tests` (tally for a
  batch mixing valid, invalid, over-budget, and post-objective calls; the
  existing "bounds requested calls" fixture covers the first three, the
  objective fixture the fourth), `metrics_writer_tests` (new field),
  `docs/logs.md`.

### Phase 1: reward and summary

- `agent/reward.{hpp,cpp}`: `RewardWeights`, `EpisodeFacts`,
  `RewardBreakdown`, `compute_reward()`, `parse_reward_weight("name=value")`.
- `agent/episode_summary.{hpp,cpp}`: `EpisodeSummary` (finish reason,
  completeness, turns, score, items eaten, tool counts, call tally, duration,
  reward) and `summarize_episode(const Session &, const RewardWeights &)`.
  `MetricsWriter::finish()` takes the summary instead of recomputing counts;
  the abandoned-footer path is unchanged. Serialisation lives in one place
  (`agent/summary_json.cpp`, private nlohmann use, exposing `std::string
  to_json_line(const EpisodeSummary &)` for front ends).
- Footer gains `reward`, `reward_version`, `reward_weights`, `calls`.
- `pig-pen-headless`: `--reward NAME=VALUE` (repeatable) and `reward=` in the
  summary line. GUI: reward total and breakdown in the Stats panel.
- Tests: `reward_tests.cpp` (each term in isolation, defaults, invalid
  episodes, weight parsing), footer assertions, integration test checks the
  breakdown for the known two-request scenario.

### Phase 2: the seams (pure refactor)

- `SessionOptions { std::optional<std::filesystem::path> log_directory;
  std::string prompt_variant; std::string api_key; std::string rollout_id;
  std::vector<std::pair<std::string,std::string>> request_headers; }`.
  `Session::create(Config, SessionOptions)`; `getenv` moves to the front ends.
  `scry_config()` forwards headers. Header line records `rollout_id`.
- `agent/episode_driver.{hpp,cpp}`: `IDrivableEpisode` (`pump()`,
  `finished()`, `stop()`), implemented by `Session`; `EpisodeDriver::step(now,
  stop_requested)` returns `finished | timed_out | cancellation_stalled |
  interrupted` and encapsulates the deadline and 15 s grace exactly as
  `headless_main.cpp` does now.
- `src/cli/` → `pigpen_cli` (no scry, no nlohmann): `OptionParser`
  (`flag/text/integer/real` registrations, both `--k v` and `--k=v`, the
  existing error texts), `add_config_options(parser, Config &,
  SessionOptions &, RewardWeights &)`, `TerminationSignal`.
  `headless_main.cpp` shrinks to usage text, its four extra options, the
  print cursor, and a `while (!driver.step(...))` loop. `gui_options.cpp`
  reuses the parser for `--model` / `--base-url` with its length caps.
- Tests: `episode_driver_tests.cpp` against a fake `IDrivableEpisode`
  (finish, timeout then grace, stall, signal); `cli_options_tests.cpp`
  replaces the parsing half of `gui_options_tests.cpp`; every existing
  headless CTest and the integration test pass unchanged.

### Phase 3: `pig-pen-worker`

- `agent/episode_batch.{hpp,cpp}`: job queue + up to P live
  `EpisodeDriver`s, generic over a `std::function` episode factory so it is
  unit-tested with fakes (ordering, parallel cap, per-episode timeout, stop
  cancels every in-flight episode, records emitted exactly once).
- `src/app/worker_main.cpp`: job expansion, rollout ids and headers, JSONL
  stdout, exit codes. Registered in CMake next to `pig-pen-headless`; built in
  the headless preset too.
- `tests/worker_integration_tests.py`: a **threaded** loopback stub (the
  current one is single-threaded) serving 3 seeds × 2 samples with
  `--parallel 3`; asserts the `X-Pigpen-Rollout` header on every request,
  that requests for one rollout share one id, six `episode` records plus one
  `batch` record, identical `items_eaten`/positions for the two samples of one
  seed (world determinism is independent of sampling), reward breakdowns, and
  exit code 0; a second scenario with one scripted error asserts `valid:
  false` and exit 6.
- Docs: `docs/training.md` (interface, record format, joining rollouts with a
  trainer's server, seed discipline, reward weights), and updates to
  `running.md`, `logs.md`, `architecture.md`, `testing.md`, `CLAUDE.md`.

### Phase 4: optional follow-ups

- `--jobs -`: read job specs (`{"seed":..,"sample":..,"rollout_id":..}`) as
  JSONL from stdin so a trainer can drive a long-lived worker without
  respawning.
- A tiny reference consumer (`examples/rollout_consumer.py`) showing how a
  server-side request log keyed by `X-Pigpen-Rollout` joins with worker
  records.
- `Session::pump()` budget parameters if P sessions × 2 ms ever matters.
- Harness reuse across episodes, only if profiling shows creation cost.

## 5. Testing strategy

| layer | how | network? |
|---|---|---|
| reward, summary, tally arithmetic | Catch2 on plain structs | no |
| driver, batch scheduling | Catch2 with fake `IDrivableEpisode` / fake factory | no |
| real bindings, tally from scry dispatch | `scry::testing` scripted transport (existing suite) | no |
| worker end to end (headers, records, exit codes, parallelism) | Python threaded loopback stub, like `headless_integration_tests.py` | loopback only |

`just ci` stays the gate; the suite still needs no model server.

## 6. Risks and open points

- **Header collisions.** Scry rejects `extra_headers` that collide with its
  own; `X-Pigpen-*` is safe. Any user-supplied `--header` is validated by
  `Harness::validate` before a log is opened, as today.
- **Stub server concurrency.** The integration stub must move to
  `ThreadingHTTPServer`; the existing single-threaded one would serialise
  parallel sessions and hide bugs.
- **Zero-tool-turn attribution** relies on `TurnRecord::tool_calls`, which
  counts activities including `result_dispatched: false`; that is the right
  count for "did the world change", and the tally's `executed` uses the same
  definition.
- **Reward hacking surface** is small because the world is closed and the
  runner keeps nudging until the budget ends, but the defaults in §2.3 are a
  starting point; the emitted counts exist so they can be re-weighted from
  logs rather than by re-running.
- **Not in scope.** Pig Pen still trains nothing itself: the policy, the
  inference server, and the optimiser are the trainer's. The worker only
  guarantees that what it plays is what `pig-pen-headless` and the GUI play.
