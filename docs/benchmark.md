# Benchmark

The benchmark runs many models through the same seeded pens and scores each
episode. It makes the leaderboard at the top of the README. The full results
are in [Leaderboard](leaderboard.md).

The benchmark is `bench/pigpen_bench.py`. It is a Python script that uses only
the standard library (Python 3.11 or later). It does not change Pig Pen. It
starts `pig-pen-headless` once for each episode, and it reads the JSONL log
that the CLI writes. For the log format, refer to [Logs](logs.md).

## What the benchmark measures

Each episode is one run of the CLI with one model, one scenario, and one world
seed. The world, the prompt, the tools, and the limits are the same as in a
manual run. For the rules, refer to [World and tools](world.md).

The benchmark measures how well a model plays with these tools:

- the score that the model gets, and if it eats all positive items
- if it eats toadstools
- if its tool calls are valid
- how fast it is, and how many tokens it writes

A model gets no help. The benchmark never sends `--input`.

## Suites

A suite is a TOML file in `bench/suites/`. It sets the seeds, the episode
settings, and the scenarios. Three suites are available:

| suite | scenarios | seeds | turns | timeout | use |
|---|---|---|---:|---:|---|
| `standard` | `default` | 1–10 | 20 | 900 s | The leaderboard of record. The README shows it. |
| `quick` | `default` | 1–3 | 6 | 600 s | A smoke check. Does the model call the tools? |
| `scenarios` | `default`, `hidden-values`, `no-reward-feedback`, `opaque-look`, `blind` | 1–5 | 20 | 900 s | The effect of each scenario flag. `blind` sets all three flags. |

All three suites use 8 tool rounds, temperature `0.0`, and sampling seed `0`.

## Run the benchmark

Before you start, start the model server and pull each model. The default
endpoint is Ollama on `http://127.0.0.1:11434/v1`.

```sh
ollama pull MODEL_A
ollama pull MODEL_B
just bench release --model MODEL_A --model MODEL_B
```

`just bench` builds `pig-pen-headless` with the `release` preset, and then it
starts `bench/pigpen_bench.py run`. Give the preset as the **first** argument,
as with `just run-headless`. The default suite is `standard`. To use a
different suite, add `--suite NAME`:

```sh
just bench release --suite quick --model MODEL_A
```

If you do not use `just`, use these commands:

```sh
cmake --preset release
cmake --build --preset release --target pig-pen-headless
python3 bench/pigpen_bench.py run --model MODEL_A --model MODEL_B
```

To give many models, write them in a file, one model on each line. A `#`
starts a comment. The benchmark ignores blank lines and duplicate models. It
keeps the sequence of the command line and the files.

```sh
just bench release --models-file models.txt
```

The benchmark runs one episode at a time. Parallel requests to one GPU would
change the latency. It runs all episodes of one model before it starts the next
model, so the server loads only one model at a time. For each model, it runs
the scenarios in sequence, and for each scenario, the seeds in sequence.

For each episode, the benchmark prints one line: the position, the model, the
scenario, the seed, the score, the finish reason, the outcome, and the
duration. At the end, it prints a summary. The CLI output goes to files in the
episode directory, not to the terminal.

### Options of `run`

| option | default | description |
|---|---|---|
| `--suite NAME` | `standard` | suite name in `bench/suites/`, or a path to a `.toml` file |
| `--model NAME` | | exact model identifier. You can give it many times. |
| `--models-file PATH` | | file with one model on each line. You can give it many times. |
| `--headless PATH` | `build/release/pig-pen-headless` | the CLI binary. `just bench` sets it. |
| `--base-url URL` | `http://127.0.0.1:11434/v1` | model endpoint. The benchmark gives it to the CLI. |
| `--runs-dir PATH` | `bench-runs` | directory for the raw runs |
| `--retry-errors` | off | runs the episodes with the outcome `error` again |
| `--force` | off | runs all episodes again |
| `--max-consecutive-errors N` | `3` | after `N` `error` episodes in a sequence for one model, skips the other episodes of that model |
| `--dry-run` | off | prints the commands and the episodes to skip, and runs nothing |
| `--no-preflight` | off | does not do the server check |

The CLI gets the `PIGPEN_API_KEY` environment variable without change.

### Preflight

Before the first episode, the benchmark sends `GET <base-url>/models`, with a
5 second time limit. If `PIGPEN_API_KEY` is set, the request sends it as a
bearer token.

- If the server does not reply, the benchmark stops before it runs an episode.
- If the server does not list a model, the benchmark shows a warning and skips
  that model. A name without a tag also matches `NAME:latest`, as in Ollama.

To disable the check, use `--no-preflight`.

### Model metadata

If the base URL ends in `/v1`, the benchmark also asks Ollama for the model
details: `POST <root>/api/show`. It records the parameter size, the
quantization level, and the family. It asks one time for each model in a run.
If the request fails, the benchmark continues, and the tables show `—`.

### Resume, retry, and force

To resume a run, use the same command again. The benchmark skips each episode
that it recorded before. An episode is recorded when its directory has an
`episode.json` file. Episodes with the outcome `error` are also recorded. To
run them again, use `--retry-errors`. To run all episodes again, use `--force`.

Before the benchmark runs an episode again, it deletes the directory of that
episode. Each episode directory always holds exactly one run.

To see the plan first, use `--dry-run`:

```sh
python3 bench/pigpen_bench.py run --dry-run --model MODEL_A --no-preflight
```

### Stop the benchmark

Push Ctrl-C, or send `SIGTERM` to the benchmark. The benchmark sends the
signal to the active CLI process, and waits until the CLI writes its log footer
and stops. Then the benchmark stops. It does not record the interrupted
episode, so the next run starts again with that episode. The benchmark exits
with `130` (`SIGINT`) or `143` (`SIGTERM`).

## Exit codes

The exit code of the CLI sets the outcome of each episode:

| CLI exit code | outcome | the benchmark |
|---|---|---|
| `0` | `ok` | records the episode |
| `5` | `no_tool_calls` | records the episode |
| `3` | `timeout` | records the episode |
| `1` | `error` | records the episode. Resume skips it, unless you use `--retry-errors`. |
| `130` / `143` | `interrupted` | does not record the episode, and stops |
| `2` | | stops. The CLI rejected the command line. Build `pig-pen-headless` from the same checkout. |
| `4` | | stops. The CLI could not write the log. Examine the free disk space. |

Other exit codes, for example a crash, have the outcome `error`.

The benchmark itself exits with these codes:

| code | description |
|---|---|
| `0` | Each scheduled episode is recorded, or the dry run finished. |
| `1` | `report` or `render` failed. |
| `2` | The command line or the suite file is not valid, or the CLI binary is not available. |
| `3` | The benchmark skipped one or more models: the preflight did not find them, or they had too many errors in a sequence. |
| `4` | The benchmark stopped: the server did not reply, or the CLI exited with `2` or `4`. |
| `130` / `143` | `SIGINT` / `SIGTERM` |

## Where results live

| path | committed | content |
|---|---|---|
| `bench-runs/<suite>-<fingerprint>/<model>/<scenario>/seed-<N>/` | no | the raw runs |
| `bench/results/<suite>.json` | yes | the summary for each suite. The leaderboard comes from these files. |
| `README.md`, `docs/leaderboard.md` | yes | the tables that `render` writes |

The fingerprint is the first 8 hexadecimal characters of a SHA-256 hash of the
suite settings. The name and the description are not part of the hash. If you
change a setting, the fingerprint changes. Then new runs go to a new directory,
and they never mix with the old runs.

The model directory is the model name with `_` in place of characters that
are not easy to use in file names, in lowercase. If this changes the name, the
benchmark adds a short hash of the exact name, for example
`llama3.1_8b-8754b77c`. Two models never use the same directory.

Each episode directory holds these files:

- the JSONL log of the CLI
- `stdout.txt` and `stderr.txt`: the CLI output
- `episode.json`: the model, the scenario, the seed, the exit code, the
  outcome, the start and end times, the wall time, the suite name and
  fingerprint, the Pig Pen commit (with `-dirty` if the tracked files have
  changes), the model metadata, the log file name, and the full command

## Scoring and ranking

The maximum score is 25: 6 berries × 1, 3 apples × 3, and 1 truffle × 10. The
benchmark calculates it from the item counts in the log header and the item
values in [World and tools](world.md#items).

The main metric is the **mean score** of a model over all scheduled episodes
of the suite. Each scheduled episode counts:

- An `error` or `timeout` episode keeps the score that its log footer recorded.
- An episode without a usable log counts as 0. A usable log has a header and a
  footer. Then the row is *incomplete*, and the tables mark the episode count
  with `*`.

The ranking uses these values, in this sequence:

1. the mean score, highest first
2. the cleared rate, highest first
3. the mean turns used, lowest first
4. the model name

## Metrics

The benchmark calculates these metrics for each model and scenario:

| metric | definition |
|---|---|
| Score | The mean `final_score` over all scheduled episodes, ± the sample standard deviation |
| Min–max | The lowest and the highest score of the scheduled episodes |
| % of max | The mean score divided by the maximum score |
| Cleared | The fraction of scheduled episodes with the finish reason `objective_complete` |
| Toadstools/ep | The toadstools eaten, divided by the scheduled episodes |
| Turns used | The mean `turns_used` of the episodes with a usable log |
| Valid calls | The sum of `tool_calls` divided by the sum of `scry_tools.calls`, over all turns. Turns with a `null` `scry_tools` value are not included. |
| Zero-tool turns | The fraction of turns with `zero_tool_turn: true` |
| Errors + timeouts | The number of episodes with the outcome `error` or `timeout` |
| Median latency | The median `latency_ms` of all turns |
| Output tokens/turn | The mean `output_tokens` of the turns that have a value |
| Episodes | The episodes with a usable log, and the scheduled episodes |

The README shows the `standard` suite, `default` scenario, with the main
columns. [Leaderboard](leaderboard.md) shows all suites, scenarios, and
columns.

### Repeatability

All suites use temperature `0` and a sampling seed. The server applies the
sampling seed only on a best-effort basis. For more data, refer to
[Run Pig Pen](running.md#options). The world seed sets the pen exactly. Thus,
most of the variance in a suite comes from the different seeds.

## Publish the leaderboard

Two subcommands make the leaderboard:

- `report` reads `bench-runs/<suite>-<fingerprint>/` for the current settings
  of the suite. It writes `bench/results/<suite>.json`, and then it does a
  `render`.
- `render` reads only the committed `bench/results/*.json` files. It writes the
  table between the `<!-- leaderboard:start -->` and `<!-- leaderboard:end -->`
  markers in the README, and all of `docs/leaderboard.md`. It does not need
  the raw runs. If you render two times, the files do not change.

```sh
just bench release --model MODEL_A --model MODEL_B
just bench-report                       # the standard suite
just bench-report --suite scenarios     # another suite
git add bench/results README.md docs/leaderboard.md
```

`report` merges the new results into the existing JSON file. The models in the
raw runs replace their old entries. The models that are only in the JSON file
stay. Thus, you can delete old raw runs, and add new models later. To remove
the models that have no raw runs, use `report --prune`.

If the suite settings changed after the last report, the old results are for
a different fingerprint. Then `report` stops. To replace the old results, use
`--prune`.

Do not edit `docs/leaderboard.md` or the text between the README markers. The
next `render` replaces them.

## Add a suite

Add a TOML file to `bench/suites/`. The file name must be the suite name:

```toml
name = "long"                    # must match long.toml
description = "Default scenario, five seeds, 50 turns."
seeds = [1, 2, 3, 4, 5]          # world seeds, 0 to 2^64 - 1, no duplicates
turns = 50                       # 1 to 10000
max_tool_rounds = 8              # 1 to 64
temperature = 0.0                # 0.0 to 2.0
sampling_seed = 0                # 0 to 4294967295; leave out to send none
timeout_seconds = 1800           # 1 to 86400, for each episode

[[scenarios]]
name = "default"

[[scenarios]]
name = "opaque-look"
opaque_look = true               # also: hidden_values, no_reward_feedback
```

Names use lowercase letters, digits, `-`, and `_`. The benchmark rejects
unknown keys and values out of range, with the name of the bad key. The CLI
gets `--prompt-variant bench:<suite>:<scenario>`, so the log header names the
suite and the scenario.
