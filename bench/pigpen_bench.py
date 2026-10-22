#!/usr/bin/env python3
"""Benchmark models with pig-pen-headless and publish a leaderboard.

The bench drives the headless CLI as a subprocess, one episode at a time, and
reads the JSONL logs that the CLI writes. It uses only the Python standard
library. For the rules and the metric definitions, refer to docs/benchmark.md.

Subcommands:
  run     run a suite for one or more models (resumes earlier runs)
  report  aggregate raw runs into bench/results/<suite>.json, then render
  render  regenerate the README leaderboard and docs/leaderboard.md
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import os
import re
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections import Counter
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from typing import Self

if sys.version_info < (3, 11):
    raise SystemExit("pigpen_bench.py needs Python 3.11 or later (tomllib)")

import tomllib

REPO_ROOT = Path(__file__).resolve().parent.parent
SUITES_DIR = REPO_ROOT / "bench" / "suites"
RESULTS_DIR = REPO_ROOT / "bench" / "results"
RUNS_DIR = REPO_ROOT / "bench-runs"
README_PATH = REPO_ROOT / "README.md"
LEADERBOARD_PATH = REPO_ROOT / "docs" / "leaderboard.md"
DEFAULT_HEADLESS = REPO_ROOT / "build" / "release" / "pig-pen-headless"
DEFAULT_BASE_URL = "http://127.0.0.1:11434/v1"

# The README shows this suite and scenario.
LEADERBOARD_SUITE = "standard"
LEADERBOARD_SCENARIO = "default"
README_START = "<!-- leaderboard:start -->"
README_END = "<!-- leaderboard:end -->"

# Item rewards from docs/world.md. This is the only place the bench keeps them.
ITEM_VALUES = {"berry": 1, "apple": 3, "truffle": 10, "toadstool": -5}
# Item counts from docs/world.md. Used only when no log header is available.
DEFAULT_ITEM_COUNTS = {"berry": 6, "apple": 3, "truffle": 1, "toadstool": 3}

# Scenario booleans in suite files, and the CLI flag that each one sets.
SCENARIO_FLAGS = {
    "hidden_values": "--hidden-values",
    "no_reward_feedback": "--no-reward-feedback",
    "opaque_look": "--opaque-look",
}
# Every pig-pen-headless flag that the bench emits. The contract test checks
# that `pig-pen-headless --help` still names each one.
VALUE_FLAGS = (
    "--base-url",
    "--model",
    "--seed",
    "--turns",
    "--max-tool-rounds",
    "--temperature",
    "--sampling-seed",
    "--timeout-seconds",
    "--log-dir",
    "--prompt-variant",
)
EMITTED_FLAGS = VALUE_FLAGS + tuple(SCENARIO_FLAGS.values())

# Ranges from docs/running.md.
SEED_MAX = 2**64 - 1
TURNS_RANGE = (1, 10_000)
TOOL_ROUNDS_RANGE = (1, 64)
TEMPERATURE_RANGE = (0.0, 2.0)
SAMPLING_SEED_RANGE = (0, 2**32 - 1)
TIMEOUT_RANGE = (1, 86_400)

# pig-pen-headless exit codes (docs/running.md) and the outcome of each.
RECORDED_OUTCOMES = {0: "ok", 5: "no_tool_calls", 3: "timeout", 1: "error"}
ABORT_EXITS = {
    2: "pig-pen-headless rejected the command line (exit 2). The bench and the "
    "binary disagree about the flags. Rebuild pig-pen-headless from this "
    "checkout.",
    4: "pig-pen-headless could not write the metrics log (exit 4). Check the "
    "free disk space and the permissions of the runs directory.",
}
INTERRUPT_EXITS = (130, 143)
INTERRUPTED = "interrupted"

# Bench exit statuses (docs/benchmark.md).
EXIT_OK = 0
EXIT_FAILURE = 1
EXIT_USAGE = 2
EXIT_MODELS_SKIPPED = 3
EXIT_ABORTED = 4

SCHEMA_VERSION = 1
HTTP_TIMEOUT_SECONDS = 5.0
NAME_PATTERN = re.compile(r"[a-z0-9][a-z0-9_-]*\Z")
MISSING = "—"


class BenchError(Exception):
    """A failure with a clear message and the exit status to use."""

    def __init__(self, message: str, status: int = EXIT_FAILURE) -> None:
        super().__init__(message)
        self.status = status


# ---------------------------------------------------------------------------
# Suites


@dataclass(frozen=True)
class Scenario:
    name: str
    hidden_values: bool = False
    no_reward_feedback: bool = False
    opaque_look: bool = False

    def flags(self) -> list[str]:
        return [flag for key, flag in SCENARIO_FLAGS.items() if getattr(self, key)]

    def settings(self) -> dict[str, Any]:
        return {
            "name": self.name,
            **{key: getattr(self, key) for key in SCENARIO_FLAGS},
        }


@dataclass(frozen=True)
class Suite:
    name: str
    description: str
    seeds: tuple[int, ...]
    turns: int
    max_tool_rounds: int
    temperature: float
    sampling_seed: int | None
    timeout_seconds: int
    scenarios: tuple[Scenario, ...]

    def settings(self) -> dict[str, Any]:
        """The canonical settings: everything except name and description."""
        return {
            "seeds": list(self.seeds),
            "turns": self.turns,
            "max_tool_rounds": self.max_tool_rounds,
            "temperature": self.temperature,
            "sampling_seed": self.sampling_seed,
            "timeout_seconds": self.timeout_seconds,
            "scenarios": [scenario.settings() for scenario in self.scenarios],
        }

    @property
    def fingerprint(self) -> str:
        return fingerprint(self.settings())

    def runs_dir(self, runs_root: Path) -> Path:
        return runs_root / f"{self.name}-{self.fingerprint}"


def fingerprint(settings: dict[str, Any]) -> str:
    canonical = json.dumps(settings, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode()).hexdigest()[:8]


def _check_keys(
    table: dict[str, Any], allowed: Iterable[str], required: Iterable[str], where: str
) -> None:
    unknown = sorted(set(table) - set(allowed))
    if unknown:
        raise BenchError(f"{where}: unknown key(s): {', '.join(unknown)}", EXIT_USAGE)
    missing = sorted(set(required) - set(table))
    if missing:
        raise BenchError(f"{where}: missing key(s): {', '.join(missing)}", EXIT_USAGE)


def _integer(value: Any, key: str, low: int, high: int, where: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BenchError(f"{where}: {key} must be an integer", EXIT_USAGE)
    if not low <= value <= high:
        raise BenchError(
            f"{where}: {key} must be in the range {low}..{high}", EXIT_USAGE
        )
    return value


def _name(value: Any, key: str, where: str) -> str:
    if not isinstance(value, str) or not NAME_PATTERN.match(value):
        raise BenchError(
            f"{where}: {key} must be lowercase letters, digits, '-' or '_', "
            "and start with a letter or digit",
            EXIT_USAGE,
        )
    return value


def parse_suite(data: dict[str, Any], where: str) -> Suite:
    """Validate a parsed suite file strictly. Errors name the bad key."""
    keys = (
        "name",
        "description",
        "seeds",
        "turns",
        "max_tool_rounds",
        "temperature",
        "sampling_seed",
        "timeout_seconds",
        "scenarios",
    )
    _check_keys(data, keys, [k for k in keys if k != "sampling_seed"], where)
    name = _name(data["name"], "name", where)
    description = data["description"]
    if not isinstance(description, str) or not description.strip():
        raise BenchError(f"{where}: description must be a non-empty string", EXIT_USAGE)

    seeds = data["seeds"]
    if not isinstance(seeds, list) or not seeds:
        raise BenchError(f"{where}: seeds must be a non-empty array", EXIT_USAGE)
    for seed in seeds:
        _integer(seed, "each seed", 0, SEED_MAX, where)
    if len(set(seeds)) != len(seeds):
        raise BenchError(f"{where}: seeds must not repeat", EXIT_USAGE)

    temperature = data["temperature"]
    if isinstance(temperature, bool) or not isinstance(temperature, (int, float)):
        raise BenchError(f"{where}: temperature must be a number", EXIT_USAGE)
    low, high = TEMPERATURE_RANGE
    if not math.isfinite(temperature) or not low <= temperature <= high:
        raise BenchError(
            f"{where}: temperature must be in the range {low}..{high}", EXIT_USAGE
        )

    sampling_seed = data.get("sampling_seed")
    if sampling_seed is not None:
        sampling_seed = _integer(
            sampling_seed, "sampling_seed", *SAMPLING_SEED_RANGE, where
        )

    scenarios = data["scenarios"]
    if not isinstance(scenarios, list) or not scenarios:
        raise BenchError(
            f"{where}: scenarios must be a non-empty array of tables", EXIT_USAGE
        )
    parsed: list[Scenario] = []
    for index, table in enumerate(scenarios):
        at = f"{where}: scenarios[{index}]"
        if not isinstance(table, dict):
            raise BenchError(f"{at} must be a table", EXIT_USAGE)
        _check_keys(table, ("name", *SCENARIO_FLAGS), ("name",), at)
        flags = {}
        for key in SCENARIO_FLAGS:
            value = table.get(key, False)
            if not isinstance(value, bool):
                raise BenchError(f"{at}: {key} must be true or false", EXIT_USAGE)
            flags[key] = value
        parsed.append(Scenario(_name(table["name"], "name", at), **flags))
    names = [scenario.name for scenario in parsed]
    if len(set(names)) != len(names):
        raise BenchError(f"{where}: scenario names must not repeat", EXIT_USAGE)

    return Suite(
        name=name,
        description=description.strip(),
        seeds=tuple(seeds),
        turns=_integer(data["turns"], "turns", *TURNS_RANGE, where),
        max_tool_rounds=_integer(
            data["max_tool_rounds"], "max_tool_rounds", *TOOL_ROUNDS_RANGE, where
        ),
        temperature=float(temperature),
        sampling_seed=sampling_seed,
        timeout_seconds=_integer(
            data["timeout_seconds"], "timeout_seconds", *TIMEOUT_RANGE, where
        ),
        scenarios=tuple(parsed),
    )


def suite_path(value: str) -> Path:
    """A suite name resolves in bench/suites/. A path to a .toml file also works."""
    if value.endswith(".toml") or "/" in value or os.sep in value:
        return Path(value)
    return SUITES_DIR / f"{value}.toml"


def load_suite(value: str) -> Suite:
    path = suite_path(value)
    if not path.is_file():
        known = sorted(p.stem for p in SUITES_DIR.glob("*.toml"))
        raise BenchError(
            f"unknown suite {value!r}: {path} does not exist "
            f"(available: {', '.join(known) or 'none'})",
            EXIT_USAGE,
        )
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as error:
        raise BenchError(f"{path}: not valid TOML: {error}", EXIT_USAGE) from error
    suite = parse_suite(data, str(path))
    if suite.name != path.stem:
        raise BenchError(
            f"{path}: name {suite.name!r} must match the file name {path.stem!r}",
            EXIT_USAGE,
        )
    return suite


# ---------------------------------------------------------------------------
# Models


def model_slug(model: str) -> str:
    """A directory name for a model. Sanitized like the pig-pen log names
    (docs/logs.md), lowercased, plus a short hash of the exact name whenever
    that changed anything, so two models never share a directory."""
    slug = re.sub(r"[^A-Za-z0-9._-]", "_", model).lower() or "model"
    if slug.startswith((".", "-")):
        slug = "_" + slug[1:]
    if slug != model:
        slug = f"{slug}-{hashlib.sha256(model.encode()).hexdigest()[:8]}"
    return slug


def check_model_name(model: str, where: str) -> str:
    if not model or model != model.strip() or "\n" in model:
        raise BenchError(f"{where}: invalid model name {model!r}", EXIT_USAGE)
    if model.startswith("-"):
        raise BenchError(f"{where}: a model name cannot start with '-'", EXIT_USAGE)
    return model


def collect_models(sources: Sequence[tuple[str, str]]) -> list[str]:
    """Models from --model and --models-file, in command-line order, without
    duplicates."""
    models: list[str] = []
    for kind, value in sources:
        if kind == "model":
            models.append(check_model_name(value, "--model"))
            continue
        path = Path(value)
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as error:
            raise BenchError(
                f"--models-file: cannot read {path}: {error}", EXIT_USAGE
            ) from error
        for number, line in enumerate(lines, start=1):
            line = line.split("#", 1)[0].strip()
            if line:
                models.append(check_model_name(line, f"{path}:{number}"))
    return list(dict.fromkeys(models))


# ---------------------------------------------------------------------------
# HTTP: preflight and model metadata


def http_json(url: str, payload: Any = None, api_key: str | None = None) -> Any:
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        url, data=data, method="GET" if data is None else "POST"
    )
    request.add_header("Accept", "application/json")
    if data is not None:
        request.add_header("Content-Type", "application/json")
    if api_key:
        request.add_header("Authorization", f"Bearer {api_key}")
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
        return json.load(response)


def list_server_models(base_url: str, api_key: str | None) -> set[str]:
    """GET <base_url>/models. Any failure is a BenchError that aborts the run."""
    url = base_url.rstrip("/") + "/models"
    try:
        body = http_json(url, api_key=api_key)
        return {str(entry["id"]) for entry in body["data"]}
    except urllib.error.HTTPError as error:
        reason = f"HTTP {error.code}"
    except (urllib.error.URLError, OSError) as error:
        reason = str(getattr(error, "reason", error))
    except (ValueError, KeyError, TypeError) as error:
        reason = f"unexpected response ({error})"
    raise BenchError(
        f"preflight: the model server at {base_url} is not usable: GET {url}: "
        f"{reason}. Start the server (for example `ollama serve`), or pass "
        "--no-preflight.",
        EXIT_ABORTED,
    )


def model_listed(model: str, available: set[str]) -> bool:
    # Ollama resolves a name without a tag to ":latest".
    return model in available or f"{model}:latest" in available


def fetch_metadata(base_url: str, model: str, api_key: str | None) -> dict | None:
    """Ollama /api/show details, best effort. None when not available."""
    base = base_url.rstrip("/")
    if not base.endswith("/v1"):
        return None
    try:
        body = http_json(base[: -len("/v1")] + "/api/show", {"model": model}, api_key)
        details = body.get("details") or {}
        metadata = {
            key: str(details[key])
            for key in ("parameter_size", "quantization_level", "family")
            if details.get(key)
        }
    except (OSError, ValueError, AttributeError, TypeError):  # never fatal
        return None
    return metadata or None


# ---------------------------------------------------------------------------
# Logs and sidecars


def utc_now() -> str:
    now = datetime.datetime.now(datetime.timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%SZ")


def pigpen_commit() -> str | None:
    """`git rev-parse --short HEAD`, plus "-dirty" for tracked changes."""
    try:
        head = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        ).stdout.strip()
        status = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "status", "--porcelain", "-uno"],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    if not head:
        return None
    return f"{head}-dirty" if status else head


def read_log(path: Path) -> dict[str, Any] | None:
    """A pig-pen JSONL log as {header, turns, tools, footer}, or None when the
    file is missing, is not valid JSONL, or has no header or footer."""
    try:
        records = [
            json.loads(line)
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    except (OSError, ValueError):
        return None
    if (
        len(records) < 2
        or not all(isinstance(record, dict) for record in records)
        or records[0].get("type") != "header"
        or records[-1].get("type") != "footer"
    ):
        return None
    return {
        "header": records[0],
        "footer": records[-1],
        "turns": [r for r in records[1:-1] if r.get("type") == "turn"],
        "tools": [r for r in records[1:-1] if r.get("type") == "tool"],
    }


def find_log(episode_dir: Path) -> Path | None:
    logs = sorted(episode_dir.glob("*.jsonl"))
    return logs[-1] if logs else None


def read_sidecar(path: Path) -> dict[str, Any] | None:
    try:
        sidecar = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return sidecar if isinstance(sidecar, dict) else None


def write_json(path: Path, value: Any) -> None:
    """Write atomically, so an interruption never leaves half a file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def is_recorded(sidecar: dict[str, Any] | None) -> bool:
    return sidecar is not None and sidecar.get("outcome") not in (None, INTERRUPTED)


def max_score(header: dict[str, Any] | None) -> int:
    """The sum of all positive item rewards in the scenario of the header."""
    items = DEFAULT_ITEM_COUNTS
    if header and isinstance(header.get("scenario", {}).get("items"), dict):
        items = header["scenario"]["items"]
    return sum(
        count * ITEM_VALUES[item]
        for item, count in items.items()
        if ITEM_VALUES.get(item, 0) > 0
    )


# ---------------------------------------------------------------------------
# run


def episode_dir(runs: Path, model: str, scenario: Scenario, seed: int) -> Path:
    return runs / model_slug(model) / scenario.name / f"seed-{seed}"


def headless_command(
    headless: Path,
    suite: Suite,
    scenario: Scenario,
    model: str,
    seed: int,
    base_url: str,
    log_dir: Path,
) -> list[str]:
    command = [
        str(headless),
        *("--base-url", base_url),
        *("--model", model),
        *("--seed", str(seed)),
        *("--turns", str(suite.turns)),
        *("--max-tool-rounds", str(suite.max_tool_rounds)),
        *("--temperature", repr(suite.temperature)),
    ]
    if suite.sampling_seed is not None:
        command += ["--sampling-seed", str(suite.sampling_seed)]
    command += [
        *("--timeout-seconds", str(suite.timeout_seconds)),
        *("--log-dir", str(log_dir)),
        *("--prompt-variant", f"bench:{suite.name}:{scenario.name}"),
        *scenario.flags(),
    ]
    return command


def format_duration(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:.1f}s"
    minutes, rest = divmod(round(seconds), 60)
    if minutes < 60:
        return f"{minutes}m{rest:02d}s"
    hours, minutes = divmod(minutes, 60)
    return f"{hours}h{minutes:02d}m"


def last_line(path: Path) -> str:
    try:
        lines = [line for line in path.read_text(errors="replace").splitlines() if line]
    except OSError:
        return ""
    return lines[-1] if lines else ""


class SignalForwarder:
    """Remembers the first SIGINT/SIGTERM and passes it on to the child.

    Ctrl-C already reaches the child through the process group. A second
    signal is harmless: pig-pen-headless cancels once.
    """

    def __init__(self) -> None:
        self.signum = 0
        self.child: subprocess.Popen[bytes] | None = None
        self._previous: dict[int, Any] = {}

    def __enter__(self) -> Self:
        for signum in (signal.SIGINT, signal.SIGTERM):
            self._previous[signum] = signal.signal(signum, self._handle)
        return self

    def __exit__(self, *exc: object) -> None:
        for signum, handler in self._previous.items():
            signal.signal(signum, handler)

    def _handle(self, signum: int, _frame: object) -> None:
        if not self.signum:
            self.signum = signum
        child = self.child
        if child is not None:
            try:
                child.send_signal(signum)
            except OSError:
                pass


@dataclass
class Plan:
    model: str
    scenario: Scenario
    seed: int
    directory: Path
    command: list[str]
    existing: dict[str, Any] | None

    def should_run(self, force: bool, retry_errors: bool) -> bool:
        if force or not is_recorded(self.existing):
            return True
        return retry_errors and self.existing.get("outcome") == "error"


def run_command(args: argparse.Namespace) -> int:
    suite = load_suite(args.suite)
    models = collect_models(args.model_sources or [])
    if not models:
        raise BenchError("run: give at least one --model or --models-file", EXIT_USAGE)
    if args.max_consecutive_errors < 1:
        raise BenchError("--max-consecutive-errors must be 1 or more", EXIT_USAGE)
    headless = Path(args.headless)
    runs = suite.runs_dir(Path(args.runs_dir))
    plans = [
        Plan(
            model=model,
            scenario=scenario,
            seed=seed,
            directory=(directory := episode_dir(runs, model, scenario, seed)),
            command=headless_command(
                headless, suite, scenario, model, seed, args.base_url, directory
            ),
            existing=read_sidecar(directory / "episode.json"),
        )
        for model in models
        for scenario in suite.scenarios
        for seed in suite.seeds
    ]
    total = len(plans)
    width = len(str(total))
    pending = sum(plan.should_run(args.force, args.retry_errors) for plan in plans)
    print(
        f"suite {suite.name} ({suite.fingerprint}): {len(models)} model(s) x "
        f"{len(suite.scenarios)} scenario(s) x {len(suite.seeds)} seed(s) = "
        f"{total} episode(s), {pending} to run, {total - pending} already recorded"
    )
    print(f"runs: {runs}")

    if args.dry_run:
        for index, plan in enumerate(plans, start=1):
            tag = f"[{index:>{width}}/{total}]"
            if plan.should_run(args.force, args.retry_errors):
                print(f"{tag} run  {shlex.join(plan.command)}")
            else:
                outcome = plan.existing.get("outcome") if plan.existing else None
                print(
                    f"{tag} skip {plan.model} {plan.scenario.name} seed={plan.seed} "
                    f"(recorded: {outcome})"
                )
        return EXIT_OK

    if not headless.is_file() or not os.access(headless, os.X_OK):
        raise BenchError(
            f"pig-pen-headless not found or not executable: {headless}. Build it "
            "with `just bench` or pass --headless PATH.",
            EXIT_USAGE,
        )
    api_key = os.environ.get("PIGPEN_API_KEY") or None
    skipped_models: list[str] = []
    if args.preflight and pending:
        available = list_server_models(args.base_url, api_key)
        for model in models:
            if not model_listed(model, available):
                print(
                    f"warning: {model!r} is not listed by {args.base_url}/models; "
                    f"skipping it (try `ollama pull {model}`)",
                    file=sys.stderr,
                )
                skipped_models.append(model)

    runs.mkdir(parents=True, exist_ok=True)
    write_json(
        runs / "suite.json",
        {"name": suite.name, "fingerprint": suite.fingerprint, **suite.settings()},
    )
    commit = pigpen_commit()
    counts: Counter[str] = Counter()
    metadata: dict[str, dict | None] = {}
    consecutive_errors: dict[str, int] = Counter()

    with SignalForwarder() as signals:
        for index, plan in enumerate(plans, start=1):
            tag = f"[{index:>{width}}/{total}]"
            label = f"{plan.model} {plan.scenario.name} seed={plan.seed}"
            if signals.signum:
                break
            if plan.model in skipped_models:
                counts["not_run"] += 1
                continue
            if not plan.should_run(args.force, args.retry_errors):
                counts["already_recorded"] += 1
                continue
            if plan.model not in metadata:
                metadata[plan.model] = fetch_metadata(
                    args.base_url, plan.model, api_key
                )

            if plan.directory.exists():
                shutil.rmtree(plan.directory)
            plan.directory.mkdir(parents=True)
            started_at = utc_now()
            start = time.monotonic()
            with (
                open(plan.directory / "stdout.txt", "wb") as stdout,
                open(plan.directory / "stderr.txt", "wb") as stderr,
            ):
                signals.child = subprocess.Popen(
                    plan.command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr
                )
                if signals.signum:  # arrived before the child was registered
                    signals.child.send_signal(signals.signum)
                exit_code = signals.child.wait()
                signals.child = None
            wall_seconds = time.monotonic() - start

            if exit_code in INTERRUPT_EXITS:
                status = exit_code if not signals.signum else 128 + signals.signum
                print(
                    f"{tag} {label}: interrupted (exit {exit_code}); not recorded. "
                    "Run the same command again to resume.",
                    file=sys.stderr,
                )
                return status
            if exit_code in ABORT_EXITS:
                detail = last_line(plan.directory / "stderr.txt")
                raise BenchError(
                    f"{tag} {label}: {ABORT_EXITS[exit_code]}"
                    + (f"\n  stderr: {detail}" if detail else ""),
                    EXIT_ABORTED,
                )

            outcome = RECORDED_OUTCOMES.get(exit_code, "error")
            log_path = find_log(plan.directory)
            log = read_log(log_path) if log_path else None
            write_json(
                plan.directory / "episode.json",
                {
                    "schema_version": SCHEMA_VERSION,
                    "model": plan.model,
                    "model_metadata": metadata[plan.model],
                    "suite": {"name": suite.name, "fingerprint": suite.fingerprint},
                    "scenario": plan.scenario.name,
                    "seed": plan.seed,
                    "exit_code": exit_code,
                    "outcome": outcome,
                    "started_at": started_at,
                    "finished_at": utc_now(),
                    "wall_seconds": round(wall_seconds, 3),
                    "pigpen_commit": commit,
                    "base_url": args.base_url,
                    "log_file": log_path.name if log_path else None,
                    "command": plan.command,
                },
            )
            counts[outcome] += 1
            footer = log["footer"] if log else {}
            score = footer.get("final_score", MISSING)
            reason = footer.get("finish_reason", MISSING)
            print(
                f"{tag} {label}: score={score} finish={reason} outcome={outcome} "
                f"{format_duration(wall_seconds)}",
                flush=True,
            )
            if outcome == "error":
                detail = last_line(plan.directory / "stderr.txt")
                if detail:
                    print(f"    stderr: {detail}", file=sys.stderr)
                consecutive_errors[plan.model] += 1
                if consecutive_errors[plan.model] >= args.max_consecutive_errors:
                    print(
                        f"warning: {consecutive_errors[plan.model]} consecutive "
                        f"errors for {plan.model!r}; skipping its remaining "
                        "episodes",
                        file=sys.stderr,
                    )
                    skipped_models.append(plan.model)
            else:
                consecutive_errors[plan.model] = 0

        if signals.signum:
            print("interrupted; run the same command again to resume.", file=sys.stderr)
            return 128 + signals.signum

    summary = ", ".join(f"{key}={counts[key]}" for key in sorted(counts)) or "nothing"
    print(f"done: {summary}")
    if skipped_models:
        print(
            "skipped model(s): " + ", ".join(repr(m) for m in skipped_models),
            file=sys.stderr,
        )
        print(f"next: python3 bench/pigpen_bench.py report --suite {suite.name}")
        return EXIT_MODELS_SKIPPED
    print(f"next: python3 bench/pigpen_bench.py report --suite {suite.name}")
    return EXIT_OK


# ---------------------------------------------------------------------------
# report: aggregation


def _rate(numerator: float, denominator: float) -> float | None:
    return numerator / denominator if denominator else None


def _round(value: float | None, digits: int = 4) -> float | None:
    return None if value is None else round(value, digits)


def scenario_metrics(
    episodes: Sequence[tuple[dict[str, Any] | None, dict[str, Any] | None]],
) -> dict[str, Any]:
    """Metrics for one (model, scenario) over all scheduled episodes.

    Each item is (sidecar, log). A scheduled episode without a recorded
    sidecar and a usable log scores 0 and marks the result incomplete.
    """
    scheduled = len(episodes)
    scores: list[float] = []
    turns_used: list[float] = []
    turns: list[dict[str, Any]] = []
    outcomes: Counter[str] = Counter()
    cleared = toadstools = recorded = 0
    best = None
    for sidecar, log in episodes:
        if is_recorded(sidecar):
            outcomes[sidecar["outcome"]] += 1
        if not is_recorded(sidecar) or log is None:
            scores.append(0)
            continue
        recorded += 1
        footer = log["footer"]
        best = best if best is not None else max_score(log["header"])
        scores.append(footer.get("final_score") or 0)
        cleared += footer.get("finish_reason") == "objective_complete"
        toadstools += (footer.get("items_eaten") or {}).get("toadstool") or 0
        if footer.get("turns_used") is not None:
            turns_used.append(footer["turns_used"])
        turns.extend(log["turns"])

    calls = valid = 0
    for turn in turns:
        stats = turn.get("scry_tools")
        if isinstance(stats, dict) and stats.get("calls") is not None:
            calls += stats["calls"]
            valid += turn.get("tool_calls") or 0
    latencies = [t["latency_ms"] for t in turns if t.get("latency_ms") is not None]
    tokens = [t["output_tokens"] for t in turns if t.get("output_tokens") is not None]
    best = best if best is not None else max_score(None)
    mean = statistics.fmean(scores) if scores else 0.0
    return {
        "episodes_scheduled": scheduled,
        "episodes_recorded": recorded,
        "incomplete": recorded < scheduled,
        "outcomes": dict(sorted(outcomes.items())),
        "max_score": best,
        "mean_score": _round(mean),
        "stdev_score": _round(statistics.stdev(scores) if len(scores) > 1 else 0.0),
        "min_score": min(scores) if scores else 0,
        "max_score_seen": max(scores) if scores else 0,
        "percent_of_max": _round(100 * mean / best if best else None),
        "cleared_rate": _round(_rate(cleared, scheduled)),
        "mean_toadstools": _round(_rate(toadstools, scheduled)),
        "mean_turns_used": _round(statistics.fmean(turns_used) if turns_used else None),
        "valid_call_rate": _round(_rate(valid, calls)),
        "zero_tool_turn_rate": _round(
            _rate(sum(bool(t.get("zero_tool_turn")) for t in turns), len(turns))
        ),
        "errors_timeouts": outcomes["error"] + outcomes["timeout"],
        "median_latency_ms": _round(
            statistics.median(latencies) if latencies else None
        ),
        "mean_output_tokens": _round(statistics.fmean(tokens) if tokens else None),
    }


def rank_key(item: tuple[str, dict[str, Any]]) -> tuple:
    """Mean score desc, cleared rate desc, mean turns used asc, model name."""
    name, metrics = item
    turns = metrics.get("mean_turns_used")
    return (
        -(metrics.get("mean_score") or 0),
        -(metrics.get("cleared_rate") or 0),
        math.inf if turns is None else turns,
        name,
    )


def ranked(models: dict[str, Any], scenario: str) -> list[tuple[str, dict, dict]]:
    """[(model, entry, metrics)] for one scenario, best first."""
    rows = [
        (name, entry["scenarios"][scenario])
        for name, entry in models.items()
        if scenario in entry.get("scenarios", {})
    ]
    return [
        (name, models[name], metrics) for name, metrics in sorted(rows, key=rank_key)
    ]


def aggregate_model(model_dir: Path, suite: Suite) -> tuple[str, dict[str, Any]] | None:
    sidecars: dict[tuple[str, int], dict[str, Any] | None] = {}
    for scenario in suite.scenarios:
        for seed in suite.seeds:
            path = model_dir / scenario.name / f"seed-{seed}" / "episode.json"
            sidecars[scenario.name, seed] = read_sidecar(path)
    recorded = [s for s in sidecars.values() if is_recorded(s)]
    if not recorded:
        return None
    names = {s.get("model") for s in recorded}
    if len(names) != 1 or not isinstance(next(iter(names)), str):
        raise BenchError(f"{model_dir}: sidecars name different models: {names}")
    model = names.pop()

    scenarios = {}
    for scenario in suite.scenarios:
        episodes = []
        for seed in suite.seeds:
            sidecar = sidecars[scenario.name, seed]
            log = None
            if is_recorded(sidecar) and sidecar.get("log_file"):
                directory = model_dir / scenario.name / f"seed-{seed}"
                log = read_log(directory / sidecar["log_file"])
            episodes.append((sidecar, log))
        scenarios[scenario.name] = scenario_metrics(episodes)

    latest = max(recorded, key=lambda s: s.get("finished_at") or "")
    with_metadata = [s for s in recorded if s.get("model_metadata")]
    metadata = (
        max(with_metadata, key=lambda s: s.get("finished_at") or "")["model_metadata"]
        if with_metadata
        else None
    )
    commits = sorted({s["pigpen_commit"] for s in recorded if s.get("pigpen_commit")})
    return model, {
        "metadata": metadata,
        "pigpen_commits": commits,
        "last_run": latest.get("finished_at"),
        "scenarios": scenarios,
    }


def report_command(args: argparse.Namespace) -> int:
    suite = load_suite(args.suite)
    runs = suite.runs_dir(Path(args.runs_dir))
    fresh: dict[str, Any] = {}
    if runs.is_dir():
        for model_dir in sorted(p for p in runs.iterdir() if p.is_dir()):
            result = aggregate_model(model_dir, suite)
            if result:
                fresh[result[0]] = result[1]
    if not fresh:
        raise BenchError(
            f"report: no recorded episodes in {runs}. Run "
            f"`python3 bench/pigpen_bench.py run --suite {suite.name} --model NAME` "
            "first."
        )

    results_path = Path(args.results_dir) / f"{suite.name}.json"
    models: dict[str, Any] = {}
    if results_path.exists() and not args.prune:
        existing = load_results(results_path)
        if existing["suite"]["fingerprint"] != suite.fingerprint:
            raise BenchError(
                f"{results_path} holds results for suite fingerprint "
                f"{existing['suite']['fingerprint']}, but the suite is now "
                f"{suite.fingerprint}. Pass --prune to replace the old results."
            )
        models = existing["models"]
    kept = sorted(set(models) - set(fresh))
    models.update(fresh)
    models = dict(sorted(models.items()))
    best = max(
        (m["max_score"] for e in models.values() for m in e["scenarios"].values()),
        default=max_score(None),
    )
    document = {
        "schema_version": SCHEMA_VERSION,
        "suite": {
            "name": suite.name,
            "description": suite.description,
            "fingerprint": suite.fingerprint,
            "settings": suite.settings(),
        },
        "generated_at": utc_now(),
        "max_score": best,
        "pigpen_commits": sorted(
            {c for e in models.values() for c in e["pigpen_commits"]}
        ),
        "models": models,
    }
    write_json(results_path, document)
    print(
        f"wrote {results_path}: {len(fresh)} model(s) from {runs}"
        + (f", {len(kept)} kept from the earlier results" if kept else "")
    )
    for scenario in suite.scenarios:
        for position, (name, _, metrics) in enumerate(
            ranked(models, scenario.name), start=1
        ):
            print(
                f"  {scenario.name} #{position} {name}: mean "
                f"{metrics['mean_score']} ({metrics['episodes_recorded']}/"
                f"{metrics['episodes_scheduled']} episodes)"
            )
    return render(args)


# ---------------------------------------------------------------------------
# render


def load_results(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise BenchError(f"{path}: cannot read results: {error}") from error
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != SCHEMA_VERSION
        or not isinstance(document.get("suite"), dict)
        or not isinstance(document.get("models"), dict)
    ):
        raise BenchError(f"{path}: not a schema_version {SCHEMA_VERSION} results file")
    return document


def load_all_results(results_dir: Path) -> list[dict[str, Any]]:
    """Every results file, the leaderboard suite first, then by name."""
    documents = [load_results(p) for p in sorted(results_dir.glob("*.json"))]
    return sorted(
        documents,
        key=lambda d: (d["suite"]["name"] != LEADERBOARD_SUITE, d["suite"]["name"]),
    )


_MARKDOWN_SPECIAL = re.compile(r"([\\`*_{}\[\]<>()#+!|~])")


def md(text: Any) -> str:
    """Escape text for a markdown table cell."""
    if text is None or text == "":
        return MISSING
    flat = " ".join(str(text).split())
    return _MARKDOWN_SPECIAL.sub(r"\\\1", flat)


def code(text: Any) -> str:
    """An inline code span. Backslash escapes do not work inside one."""
    return "`" + str(text).replace("`", "'").replace("|", "/") + "`"


def number(value: float | None, digits: int = 1, suffix: str = "") -> str:
    return MISSING if value is None else f"{value:.{digits}f}{suffix}"


def percent(rate: float | None) -> str:
    return MISSING if rate is None else f"{rate * 100:.0f}%"


def seed_list(seeds: Sequence[int]) -> str:
    """Compress seeds into ranges: [1, 2, 3, 7] -> "1–3, 7"."""
    parts: list[str] = []
    ordered = sorted(seeds)
    start = previous = ordered[0]
    for seed in [*ordered[1:], None]:
        if seed is not None and seed == previous + 1:
            previous = seed
            continue
        parts.append(str(start) if start == previous else f"{start}–{previous}")
        if seed is not None:
            start = previous = seed
    return ", ".join(parts)


def settings_line(document: dict[str, Any]) -> str:
    settings = document["suite"]["settings"]
    sampling = settings.get("sampling_seed")
    commits = ", ".join(code(c) for c in document.get("pigpen_commits", []))
    return (
        f"Seeds {seed_list(settings['seeds'])}, {settings['turns']} turns, "
        f"{settings['max_tool_rounds']} tool rounds, temperature "
        f"{settings['temperature']}, sampling seed "
        f"{MISSING if sampling is None else sampling}, max score "
        f"{document.get('max_score', max_score(None))}. Pig Pen "
        f"{commits or MISSING}. Updated {document['generated_at'][:10]}."
    )


def episodes_cell(metrics: dict[str, Any]) -> str:
    cell = f"{metrics['episodes_recorded']}/{metrics['episodes_scheduled']}"
    return cell + "\\*" if metrics.get("incomplete") else cell


def metadata_cells(entry: dict[str, Any]) -> list[str]:
    metadata = entry.get("metadata") or {}
    return [md(metadata.get("parameter_size")), md(metadata.get("quantization_level"))]


def table(
    header: Sequence[str], align: Sequence[str], rows: Iterable[Sequence[str]]
) -> str:
    lines = [
        "| " + " | ".join(header) + " |",
        "|" + "|".join("---:" if a == "r" else "---" for a in align) + "|",
    ]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines)


INCOMPLETE_NOTE = (
    "\\* Incomplete: some scheduled episodes have no recorded log. They count as 0."
)


def readme_section(documents: list[dict[str, Any]]) -> str:
    document = next(
        (d for d in documents if d["suite"]["name"] == LEADERBOARD_SUITE), None
    )
    names = (
        [s["name"] for s in document["suite"]["settings"]["scenarios"]]
        if document
        else []
    )
    if not document or not names:
        return "No results yet — see [docs/benchmark.md](docs/benchmark.md)."
    scenario = LEADERBOARD_SCENARIO if LEADERBOARD_SCENARIO in names else names[0]
    rows = ranked(document["models"], scenario)
    if not rows:
        return "No results yet — see [docs/benchmark.md](docs/benchmark.md)."
    body = table(
        [
            "#",
            "Model",
            "Params",
            "Quant",
            "Score",
            "% of max",
            "Cleared",
            "Toadstools/ep",
            "Valid calls",
            "Episodes",
        ],
        "rlllrrrrrr",
        (
            [
                str(position),
                md(name),
                *metadata_cells(entry),
                f"{number(m['mean_score'])} ± {number(m['stdev_score'])}",
                number(m["percent_of_max"], suffix="%"),
                percent(m["cleared_rate"]),
                number(m["mean_toadstools"]),
                percent(m["valid_call_rate"]),
                episodes_cell(m),
            ]
            for position, (name, entry, m) in enumerate(rows, start=1)
        ),
    )
    parts = [body, ""]
    if any(m.get("incomplete") for _, _, m in rows):
        parts += [INCOMPLETE_NOTE, ""]
    parts.append(
        f"Suite {code(document['suite']['name'])}, scenario {code(scenario)}. "
        + settings_line(document)
        + " Refer to [Benchmark](docs/benchmark.md) for the rules and to "
        "[Leaderboard](docs/leaderboard.md) for all metrics."
    )
    return "\n".join(parts)


LEADERBOARD_HEADER = """\
<!-- Generated by bench/pigpen_bench.py from bench/results/*.json. Do not edit.
     To update this page, run: python3 bench/pigpen_bench.py render -->

# Leaderboard

This page shows all committed benchmark results. One section shows one suite,
and one table shows one scenario. For the rules and the definition of each
column, refer to [Benchmark](benchmark.md#metrics).
"""


def leaderboard_page(documents: list[dict[str, Any]]) -> str:
    parts = [LEADERBOARD_HEADER]
    if not documents:
        parts.append("No results yet — see [benchmark.md](benchmark.md).\n")
        return "\n".join(parts)
    for document in documents:
        suite = document["suite"]
        parts.append(f"## Suite {code(suite['name'])}\n")
        parts.append(
            f"{md(suite.get('description'))} Fingerprint {code(suite['fingerprint'])}. "
            + settings_line(document)
            + "\n"
        )
        for scenario in suite["settings"]["scenarios"]:
            flags = [SCENARIO_FLAGS[k] for k in SCENARIO_FLAGS if scenario.get(k)]
            parts.append(f"### Scenario {code(scenario['name'])}\n")
            parts.append(
                "Flags: " + (", ".join(f"`{f}`" for f in flags) or "none") + ".\n"
            )
            rows = ranked(document["models"], scenario["name"])
            if not rows:
                parts.append("No results.\n")
                continue
            parts.append(
                table(
                    [
                        "#",
                        "Model",
                        "Params",
                        "Quant",
                        "Family",
                        "Score",
                        "Min–max",
                        "% of max",
                        "Cleared",
                        "Toadstools/ep",
                        "Turns used",
                        "Valid calls",
                        "Zero-tool turns",
                        "Errors + timeouts",
                        "Median latency",
                        "Output tokens/turn",
                        "Episodes",
                    ],
                    "rllllrrrrrrrrrrrr",
                    (
                        [
                            str(position),
                            md(name),
                            *metadata_cells(entry),
                            md((entry.get("metadata") or {}).get("family")),
                            f"{number(m['mean_score'])} ± {number(m['stdev_score'])}",
                            f"{m['min_score']}–{m['max_score_seen']}",
                            number(m["percent_of_max"], suffix="%"),
                            percent(m["cleared_rate"]),
                            number(m["mean_toadstools"]),
                            number(m["mean_turns_used"]),
                            percent(m["valid_call_rate"]),
                            percent(m["zero_tool_turn_rate"]),
                            str(m["errors_timeouts"]),
                            MISSING
                            if m["median_latency_ms"] is None
                            else f"{m['median_latency_ms'] / 1000:.1f} s",
                            number(m["mean_output_tokens"], 0),
                            episodes_cell(m),
                        ]
                        for position, (name, entry, m) in enumerate(rows, start=1)
                    ),
                )
                + "\n"
            )
            if any(m.get("incomplete") for _, _, m in rows):
                parts.append(INCOMPLETE_NOTE + "\n")
    return "\n".join(parts)


def splice(text: str, content: str, where: str) -> str:
    """Replace the text between the leaderboard markers."""
    starts, ends = text.count(README_START), text.count(README_END)
    if starts != 1 or ends != 1:
        raise BenchError(
            f"{where}: expected exactly one {README_START} and one {README_END} "
            f"marker, found {starts} and {ends}"
        )
    head, rest = text.split(README_START)
    if README_END not in rest:
        raise BenchError(f"{where}: {README_END} comes before {README_START}")
    _, tail = rest.split(README_END)
    return f"{head}{README_START}\n\n{content}\n\n{README_END}{tail}"


def update_file(path: Path, text: str) -> None:
    try:
        current = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        current = None
    if current == text:
        print(f"unchanged {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    print(f"updated {path}")


def render(args: argparse.Namespace) -> int:
    documents = load_all_results(Path(args.results_dir))
    readme = Path(args.readme)
    try:
        text = readme.read_text(encoding="utf-8")
    except OSError as error:
        raise BenchError(f"cannot read {readme}: {error}") from error
    # Check both outputs before writing either one.
    spliced = splice(text, readme_section(documents), str(readme))
    page = leaderboard_page(documents)
    update_file(readme, spliced)
    update_file(Path(args.leaderboard), page)
    return EXIT_OK


# ---------------------------------------------------------------------------
# Command line


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pigpen_bench.py",
        description="Benchmark models with pig-pen-headless and publish a "
        "leaderboard. Refer to docs/benchmark.md.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="run a suite (resumes earlier runs)")
    run.add_argument(
        "--suite", default=LEADERBOARD_SUITE, help="suite name or .toml path"
    )
    run.add_argument(
        "--model",
        dest="model_sources",
        action="append",
        type=lambda value: ("model", value),
        metavar="NAME",
        help="exact model identifier (repeatable)",
    )
    run.add_argument(
        "--models-file",
        dest="model_sources",
        action="append",
        type=lambda value: ("file", value),
        metavar="PATH",
        help="file with one model per line; '#' starts a comment (repeatable)",
    )
    run.add_argument(
        "--headless",
        default=str(DEFAULT_HEADLESS),
        help="pig-pen-headless binary (default: build/release/pig-pen-headless)",
    )
    run.add_argument("--base-url", default=DEFAULT_BASE_URL, help="model endpoint")
    run.add_argument("--runs-dir", default=str(RUNS_DIR), help="raw run directory")
    run.add_argument(
        "--retry-errors", action="store_true", help="run 'error' episodes again"
    )
    run.add_argument("--force", action="store_true", help="run every episode again")
    run.add_argument(
        "--max-consecutive-errors",
        type=int,
        default=3,
        metavar="N",
        help="skip a model after N errors in a row (default: 3)",
    )
    run.add_argument(
        "--dry-run", action="store_true", help="print the plan and run nothing"
    )
    run.add_argument(
        "--no-preflight",
        dest="preflight",
        action="store_false",
        help="do not check <base-url>/models first",
    )
    run.set_defaults(handler=run_command)

    def output_options(sub: argparse.ArgumentParser) -> None:
        sub.add_argument("--results-dir", default=str(RESULTS_DIR))
        sub.add_argument("--readme", default=str(README_PATH))
        sub.add_argument("--leaderboard", default=str(LEADERBOARD_PATH))

    report = commands.add_parser(
        "report", help="aggregate raw runs into bench/results/<suite>.json"
    )
    report.add_argument(
        "--suite", default=LEADERBOARD_SUITE, help="suite name or .toml path"
    )
    report.add_argument("--runs-dir", default=str(RUNS_DIR), help="raw run directory")
    report.add_argument(
        "--prune",
        action="store_true",
        help="drop models that have no runs instead of keeping them",
    )
    output_options(report)
    report.set_defaults(handler=report_command)

    render_parser = commands.add_parser(
        "render", help="regenerate the README leaderboard and docs/leaderboard.md"
    )
    output_options(render_parser)
    render_parser.set_defaults(handler=render)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except BenchError as error:
        print(f"error: {error}", file=sys.stderr)
        return error.status
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 128 + signal.SIGINT


if __name__ == "__main__":
    raise SystemExit(main())
