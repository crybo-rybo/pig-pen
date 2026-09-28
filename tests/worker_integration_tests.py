"""Drive pig-pen-worker end to end against a threaded loopback stub.

Covers what the C++ suite cannot: parallel sessions over the real curl/HTTP
path, the rollout and seed headers on every request, the JSONL records on
stdout, per-episode logs, jobs read from stdin with `--jobs -` (including
rejected lines and a producer that waits for each record before writing the
next job), and exit codes 0, 1 (stdout closed), 2, 6, and 130/143.

The stub is a ThreadingHTTPServer: a single-threaded one would serialise the
worker's parallel sessions and hide scheduling bugs. It scripts each rollout
independently, keyed by its X-Pigpen-Rollout header.
"""

from __future__ import annotations

import json
import math
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
from collections.abc import Callable
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from headless_integration_tests import (
    DEFAULT_REWARD_WEIGHTS,
    check,
    sse,
    subset,
    text_stream,
)

MODEL = "worker-integration-model"
PREFIX = "run42"
SAMPLING_SEED_BASE = 7
TRAINER_HEADER = ("X-Trainer-Run", "trial-9")
REWARD_TERMS = {
    "score": "score",
    "explored_cell": "explored_cells",
    "active_turn": "active_turns",
    "zero_tool_turn": "zero_tool_turns",
    "failed_action": "failed_actions",
    "invalid_call": "invalid_calls",
    "budget_refused_call": "budget_refused_calls",
    "unused_turn": "unused_turns",
}
# How long the stub waits for a second rollout before declaring the run
# serial; generous, since only a broken worker ever waits the whole time.
OVERLAP_TIMEOUT = 10


@dataclass(frozen=True)
class Request:
    path: str
    rollout: str | None
    seed: str | None
    trainer: str | None
    body: dict[str, Any]


# A scripted reply: ("stream", bytes), ("status", code), ("hang",), or
# ("gated", bytes), which streams once the stub's gate opens.
Reply = tuple[Any, ...]
Script = Callable[[str, int], Reply]


def batch_stream(calls: list[tuple[str, str, dict[str, Any]]]) -> bytes:
    """One streamed completion requesting several tool calls in one round."""
    tool_calls = [
        {
            "index": index,
            "id": call_id,
            "type": "function",
            "function": {"name": name, "arguments": json.dumps(arguments)},
        }
        for index, (call_id, name, arguments) in enumerate(calls)
    ]
    return sse({"role": "assistant", "tool_calls": tool_calls}, "tool_calls")


MOVE_AND_EAT = batch_stream(
    [
        ("call-move", "move", {"direction": "east"}),
        ("call-eat", "eat", {}),
    ]
)


def play_one_turn(_rollout: str, index: int) -> Reply:
    """Move east and eat in one round, then answer in text."""
    if index == 0:
        return ("stream", MOVE_AND_EAT)
    if index == 1:
        return ("stream", text_stream("Moved and ate."))
    return ("status", 500)


class StubServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, script: Script, overlap: int = 0) -> None:
        super().__init__(("127.0.0.1", 0), StubHandler)
        self.script = script
        self.overlap = overlap
        self.condition = threading.Condition()
        self.requests: list[Request] = []
        self.counts: dict[str, int] = {}
        self.in_flight: dict[str, int] = {}
        self.peak_rollouts = 0
        self.overlapped = threading.Event()
        self.release = threading.Event()
        self.gate = threading.Event()

    def wait_for_rollouts(self, count: int, timeout: float) -> bool:
        with self.condition:
            return self.condition.wait_for(
                lambda: len(self.counts) >= count, timeout=timeout
            )


class StubHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: StubServer

    def do_POST(self) -> None:
        stub = self.server
        self.left = False
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length))
        rollout = self.headers.get("X-Pigpen-Rollout")
        key = rollout or ""
        with stub.condition:
            index = stub.counts.get(key, 0)
            stub.counts[key] = index + 1
            stub.requests.append(
                Request(
                    self.path,
                    rollout,
                    self.headers.get("X-Pigpen-Seed"),
                    self.headers.get(TRAINER_HEADER[0]),
                    body,
                )
            )
            stub.in_flight[key] = stub.in_flight.get(key, 0) + 1
            live = sum(1 for n in stub.in_flight.values() if n > 0)
            stub.peak_rollouts = max(stub.peak_rollouts, live)
            if stub.overlap and stub.peak_rollouts >= stub.overlap:
                stub.overlapped.set()
            stub.condition.notify_all()
        try:
            # Hold each rollout's first request until enough rollouts are in
            # flight at once. An event with a timeout, not a sleep: a worker
            # that runs sessions serially just leaves the event unset.
            if stub.overlap and index == 0:
                stub.overlapped.wait(timeout=OVERLAP_TIMEOUT)
            self.reply(stub.script(key, index), key)
        finally:
            self.leave(key)

    def leave(self, key: str) -> None:
        """Stop counting this request as in flight; idempotent per request.

        Called before a response is written: once the worker can read it, it
        may start its next job while this handler thread is still running.
        """
        if self.left:
            return
        self.left = True
        with self.server.condition:
            self.server.in_flight[key] -= 1

    def reply(self, reply: Reply, key: str) -> None:
        kind = reply[0]
        if kind == "hang":
            # Never answer; the worker's timeout or signal must cancel it.
            self.server.release.wait(timeout=60)
            self.close_connection = True
            return
        if kind == "gated":
            self.server.gate.wait(timeout=60)
        self.leave(key)
        if kind == "status":
            body = b'{"error":{"message":"scripted failure"}}'
            self.send_response(reply[1])
            self.send_header("Content-Type", "application/json")
        else:
            body = reply[1]
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def log_message(self, format: str, *args: object) -> None:
        pass


class Stub:
    """A running StubServer, shut down on exit."""

    def __init__(self, script: Script, overlap: int = 0) -> None:
        self.server = StubServer(script, overlap)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> StubServer:
        self.thread.start()
        return self.server

    def __exit__(self, *_: object) -> None:
        self.server.release.set()
        self.server.gate.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def base_url(server: StubServer) -> str:
    return f"http://127.0.0.1:{server.server_port}/v1"


def worker_command(executable: str, server: StubServer, *extra: str) -> list[str]:
    return [
        executable,
        *("--base-url", base_url(server), "--model", MODEL),
        *("--turns", "1", "--max-tool-rounds", "2"),
        *extra,
    ]


def parse_output(completed_stdout: str, output: str) -> list[dict[str, Any]]:
    """Every stdout line is one JSON record; nothing else is on stdout."""
    check(completed_stdout.endswith("\n"), f"unterminated stdout\n{output}")
    lines = completed_stdout.splitlines()
    try:
        return [json.loads(line) for line in lines]
    except json.JSONDecodeError as error:
        raise AssertionError(f"stdout is not pure JSONL: {error}\n{output}")


def split_records(
    records: list[dict[str, Any]], output: str
) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    """Episode records by rollout id, and the final batch record."""
    check(len(records) >= 1, f"no records\n{output}")
    *episodes, batch = records
    check(batch.get("type") == "batch", f"last record is not a batch\n{output}")
    check(
        all(r.get("type") == "episode" for r in episodes),
        f"non-episode record before the batch\n{output}",
    )
    by_rollout = {r["rollout_id"]: r for r in episodes}
    check(len(by_rollout) == len(episodes), f"duplicate rollout ids\n{output}")
    return by_rollout, batch


def check_reward_terms(record: dict[str, Any], weights: dict[str, float]) -> None:
    """A valid reward: each term is its count times its weight, summed."""
    reward = record["reward"]
    check(reward["valid"] is True, f"reward invalid: {record!r}")
    check(reward["invalid_reason"] is None, f"reason on a valid reward: {reward!r}")
    check(record["reward_version"] == 1, f"reward version: {record!r}")
    check(record["reward_weights"] == weights, f"weights: {record!r}")
    terms = reward["terms"]
    check(terms.keys() == weights.keys(), f"reward terms: {terms!r}")
    for name, count_key in REWARD_TERMS.items():
        expected = reward[count_key] * weights[name]
        check(math.isclose(terms[name], expected, abs_tol=1e-9), f"{name}: {reward!r}")
    objective = weights["objective"] if reward["objective_complete"] else 0.0
    check(math.isclose(terms["objective"], objective, abs_tol=1e-9), f"{reward!r}")
    check(
        math.isclose(reward["total"], sum(terms.values()), abs_tol=1e-9),
        f"reward total: {reward!r}",
    )


def tool_results(requests: list[Request], rollout: str) -> list[Any]:
    """The tool results the worker posted back for @p rollout, in order."""
    return [
        json.loads(message["content"])
        for request in requests
        if request.rollout == rollout
        for message in request.body["messages"]
        if message.get("role") == "tool"
    ]


def test_parallel_batch(executable: str) -> None:
    """3 seeds x 2 samples, 3 in flight: headers, records, determinism."""
    seeds = [11, 12, 13]
    weights = DEFAULT_REWARD_WEIGHTS | {"invalid_call": -2.0}
    with Stub(play_one_turn, overlap=3) as server:
        with tempfile.TemporaryDirectory(prefix="pigpen-worker-") as log_dir:
            completed = subprocess.run(
                worker_command(
                    executable,
                    server,
                    *("--seeds", "11", "--seeds=12-13", "--samples", "2"),
                    *("--parallel", "3", "--rollout-prefix", PREFIX),
                    *("--sampling-seed-base", str(SAMPLING_SEED_BASE)),
                    *("--reward", "invalid_call=-2", "--timeout-seconds", "15"),
                    *("--header", "=".join(TRAINER_HEADER), "--log-dir", log_dir),
                ),
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
            logs = [
                [json.loads(line) for line in path.read_text().splitlines()]
                for path in pathlib.Path(log_dir).glob("*.jsonl")
            ]
        requests = list(server.requests)
        peak = server.peak_rollouts

    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 0, f"exit {completed.returncode}\n{output}")
    # The stub held each first request until three rollouts were in flight:
    # the cap was reached, and never exceeded.
    check(peak == 3, f"expected 3 rollouts in flight at the peak, got {peak}")
    episodes, batch = split_records(parse_output(completed.stdout, output), output)

    expected_ids = {f"{PREFIX}/{seed}/{sample}" for seed in seeds for sample in (0, 1)}
    check(episodes.keys() == expected_ids, f"rollout ids: {sorted(episodes)}")
    expected_batch = {
        "type": "batch",
        "status": "completed",
        "jobs": 6,
        "episodes": 6,
        "valid": 6,
        "invalid": 0,
        "not_started": 0,
        "error": None,
        "exit_code": 0,
    }
    check(subset(batch, expected_batch) == expected_batch, f"batch: {batch!r}")
    check(batch["duration_ms"] >= 0, f"batch duration: {batch!r}")

    # Every request names its rollout and seed; each rollout is one episode
    # of exactly two requests, all under one id.
    for request in requests:
        check(request.path == "/v1/chat/completions", f"endpoint: {request.path}")
        check(request.rollout in expected_ids, f"rollout header: {request!r}")
        _, seed, sample = request.rollout.split("/")
        check(request.seed == seed, f"seed header: {request!r}")
        check(request.trainer == TRAINER_HEADER[1], f"extra header: {request!r}")
        check(request.body.get("model") == MODEL, f"model: {request.body!r}")
        check(
            request.body.get("seed") == SAMPLING_SEED_BASE + int(sample),
            f"sampling seed: {request.body.get('seed')!r} for {request.rollout}",
        )
    counts = {rollout: 0 for rollout in expected_ids}
    for request in requests:
        counts[request.rollout] += 1
    check(set(counts.values()) == {2}, f"requests per rollout: {counts!r}")

    for rollout, record in episodes.items():
        _, seed, sample = rollout.split("/")
        expected = {
            "seed": int(seed),
            "sample": int(sample),
            "sampling_seed": SAMPLING_SEED_BASE + int(sample),
            "complete": True,
            "finish_reason": "turn_budget",
            "error": "",
            "turns_used": 1,
            "tool_call_counts": {"eat": 1, "look": 0, "move": 1},
            "calls": {
                "executed": 2,
                "invalid": 0,
                "budget_refused": 0,
                "host_refused": 0,
            },
        }
        check(subset(record, expected) == expected, f"{rollout}: {record!r}")
        config = record["config"]
        check(config["seed"] == int(seed), f"config seed: {config!r}")
        check(config["model"] == MODEL, f"config model: {config!r}")
        check(config["sampling_seed"] == expected["sampling_seed"], f"{config!r}")
        check(config["scenario"]["turn_budget"] == 1, f"config budget: {config!r}")
        check(record["reward"]["active_turns"] == 1, f"{rollout}: {record!r}")
        check(record["reward"]["explored_cells"] >= 1, f"{rollout}: {record!r}")
        check_reward_terms(record, weights)

    # The world is seed-deterministic whatever the sampling: both samples of
    # a seed see the same tool results and end in the same state.
    same = ("items_eaten", "final_score", "reward")
    for seed in seeds:
        first, second = (episodes[f"{PREFIX}/{seed}/{k}"] for k in (0, 1))
        check(subset(first, same) == subset(second, same), f"seed {seed} diverged")
        results = [tool_results(requests, f"{PREFIX}/{seed}/{k}") for k in (0, 1)]
        check(len(results[0]) == 2, f"tool results: {results[0]!r}")
        check(results[0] == results[1], f"seed {seed} positions: {results!r}")
        check(results[0][0]["position"] == {"x": 6, "y": 5}, f"{results!r}")

    # --log-dir writes the same per-episode logs pig-pen-headless does, one
    # per rollout, each footer agreeing with its episode record.
    check(len(logs) == 6, f"expected six logs, found {len(logs)}")
    for records in logs:
        header, footer = records[0], records[-1]
        rollout = header.get("rollout_id")
        check(rollout in episodes, f"log header rollout: {header!r}")
        check(footer.get("rollout_id") == rollout, f"log footer: {footer!r}")
        check(footer["reward"] == episodes[rollout]["reward"], f"{rollout} footer")
        check(header["seed"] == episodes[rollout]["seed"], f"{rollout} header")


def test_invalid_episodes_exit_6(executable: str) -> None:
    """A provider error and a timeout are invalid records and exit 6."""

    def script(rollout: str, index: int) -> Reply:
        seed = rollout.split("/")[1]
        if seed == "21":
            return ("status", 400)
        if seed == "22":
            return ("hang",)
        return play_one_turn(rollout, index)

    with (
        Stub(script) as server,
        tempfile.TemporaryDirectory(prefix="pigpen-worker-cwd-") as cwd,
    ):
        completed = subprocess.run(
            worker_command(
                executable,
                server,
                *("--seeds", "21-23", "--parallel", "3"),
                *("--timeout-seconds", "5"),
            ),
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
            cwd=cwd,
        )
        written = list(pathlib.Path(cwd).iterdir())

    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 6, f"exit {completed.returncode}\n{output}")
    check(written == [], f"logs are off by default, found {written!r}")
    episodes, batch = split_records(parse_output(completed.stdout, output), output)
    check(
        episodes.keys() == {"rollout/21/0", "rollout/22/0", "rollout/23/0"},
        f"rollout ids: {sorted(episodes)}\n{output}",
    )

    failed = episodes["rollout/21/0"]
    check(failed["finish_reason"] == "error", f"error record: {failed!r}")
    check(failed["error"] != "", f"error text missing: {failed!r}")
    expected_reward = {"valid": False, "invalid_reason": "error", "total": None}
    check(subset(failed["reward"], expected_reward) == expected_reward, f"{failed!r}")

    # The timeout wins over the cancellation it caused.
    timed_out = episodes["rollout/22/0"]
    expected_reward = {"valid": False, "invalid_reason": "timeout", "total": None}
    check(
        subset(timed_out["reward"], expected_reward) == expected_reward,
        f"timeout record: {timed_out!r}",
    )
    check(timed_out["sampling_seed"] is None, f"unset sampling seed: {timed_out!r}")

    check(episodes["rollout/23/0"]["reward"]["valid"] is True, f"{output}")
    expected_batch = {
        "status": "completed",
        "jobs": 3,
        "episodes": 3,
        "valid": 1,
        "invalid": 2,
        "not_started": 0,
        "exit_code": 6,
    }
    check(subset(batch, expected_batch) == expected_batch, f"batch: {batch!r}")
    for line in ("rollout/21/0 invalid (error)", "rollout/22/0 invalid (timeout)"):
        check(line in completed.stderr, f"missing {line!r} on stderr\n{output}")


def gated_script(rollout: str, index: int) -> Reply:
    """Seed 41 plays at once, 42 once the gate opens, the rest never."""
    seed = rollout.split("/")[1]
    if seed == "41":
        return play_one_turn(rollout, index)
    if seed == "42":
        return ("gated", MOVE_AND_EAT) if index == 0 else play_one_turn(rollout, 1)
    return ("hang",)


def test_closed_stdout_exits_1(executable: str) -> None:
    """A consumer that closes stdout stops the batch cooperatively."""
    with (
        Stub(gated_script) as server,
        tempfile.TemporaryDirectory(prefix="pigpen-worker-logs-") as log_dir,
    ):
        process = subprocess.Popen(
            worker_command(
                executable,
                server,
                *("--seeds", "41-44", "--parallel", "2"),
                *("--timeout-seconds", "60", "--log-dir", log_dir),
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        watchdog = threading.Timer(45, process.kill)
        watchdog.start()
        try:
            first = json.loads(process.stdout.readline())
            check(first.get("rollout_id") == "rollout/41/0", f"first: {first!r}")
            # 42 waits at the gate and 43 hangs: two episodes in flight.
            check(server.wait_for_rollouts(3, timeout=15), "43 never started")
            process.stdout.close()
            server.gate.set()
            stderr = process.stderr.read()
            process.wait(timeout=30)
        finally:
            watchdog.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()
        logs = [
            [json.loads(line) for line in path.read_text().splitlines()]
            for path in pathlib.Path(log_dir).glob("*.jsonl")
        ]
        rollouts = set(server.counts)

    check(process.returncode == 1, f"exit {process.returncode}\nstderr={stderr}")
    check("output error" in stderr, f"no output diagnostic\nstderr={stderr}")
    expected = {"rollout/41/0", "rollout/42/0", "rollout/43/0"}
    check(rollouts == expected, f"44 must not start: {sorted(rollouts)}")
    # Every started episode, including the one cancelled because stdout
    # failed, still finished and wrote its footer.
    footers = {records[0]["rollout_id"]: records[-1] for records in logs}
    check(footers.keys() == expected, f"logs: {sorted(footers)}")
    reasons = {
        rollout: footer.get("finish_reason") for rollout, footer in footers.items()
    }
    check(
        reasons
        == {
            "rollout/41/0": "turn_budget",
            "rollout/42/0": "turn_budget",
            "rollout/43/0": "stopped",
        },
        f"footers: {reasons!r}",
    )
    check(all(f.get("complete") is True for f in footers.values()), f"{footers!r}")


def test_usage_errors_exit_2(executable: str) -> None:
    for arguments, diagnostic in (
        (["--model", "m"], "--seeds is required"),
        (["--model", "m", "--seeds", "1", "--seed", "2"], "--seed is not accepted"),
        (["--model", "m", "--seeds", "3-1"], "must not descend"),
        (
            ["--model", "m", "--jobs", "-", "--seeds", "1"],
            "--jobs - cannot be combined with --seeds",
        ),
        (
            ["--model", "m", "--jobs", "-", "--sampling-seed-base", "1"],
            "--jobs - cannot be combined with --sampling-seed-base",
        ),
        (["--model", "m", "--jobs", "jobs.jsonl"], "--jobs accepts only -"),
    ):
        completed = subprocess.run(
            [executable, *arguments],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
        check(completed.returncode == 2, f"exit {completed.returncode}\n{output}")
        check(completed.stdout == "", f"usage error wrote records\n{output}")
        check(diagnostic in completed.stderr, f"missing {diagnostic!r}\n{output}")


def test_signal_cancels_in_flight(executable: str, signal_number: int) -> None:
    """A signal stops the batch: in-flight records, then the batch record."""
    with Stub(lambda _rollout, _index: ("hang",)) as server:
        process = subprocess.Popen(
            worker_command(
                executable,
                server,
                *("--seeds", "31-34", "--parallel", "2"),
                *("--timeout-seconds", "60"),
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            # Both in-flight rollouts have a request open before the signal.
            if not server.wait_for_rollouts(2, timeout=15):
                raise AssertionError("worker never opened two rollouts")
            process.send_signal(signal_number)
            stdout, stderr = process.communicate(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        rollouts = set(server.counts)

    output = f"stdout={stdout}\nstderr={stderr}"
    expected_exit = 128 + signal_number
    check(process.returncode == expected_exit, f"exit {process.returncode}\n{output}")
    check("received signal" in stderr, f"no signal diagnostic\n{output}")
    episodes, batch = split_records(parse_output(stdout, output), output)
    check(episodes.keys() == rollouts, f"{sorted(episodes)} vs {sorted(rollouts)}")
    check(len(episodes) == 2, f"expected two in-flight records\n{output}")
    for record in episodes.values():
        reward = record["reward"]
        check(reward["valid"] is False, f"interrupted reward: {record!r}")
        check(reward["invalid_reason"] == "stopped", f"reason: {record!r}")
    expected_batch = {
        "status": "interrupted",
        "jobs": 4,
        "episodes": 2,
        "valid": 0,
        "invalid": 2,
        "not_started": 2,
        "exit_code": expected_exit,
    }
    check(subset(batch, expected_batch) == expected_batch, f"batch: {batch!r}")


def split_stream_records(
    records: list[dict[str, Any]], output: str
) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]], dict[str, Any]]:
    """Episode records by rollout id, job errors in order, and the batch."""
    check(len(records) >= 1, f"no records\n{output}")
    *body, batch = records
    check(batch.get("type") == "batch", f"last record is not a batch\n{output}")
    episodes = [r for r in body if r.get("type") == "episode"]
    errors = [r for r in body if r.get("type") == "job_error"]
    check(len(episodes) + len(errors) == len(body), f"stray record\n{output}")
    by_rollout = {r["rollout_id"]: r for r in episodes}
    check(len(by_rollout) == len(episodes), f"duplicate rollout ids\n{output}")
    return by_rollout, errors, batch


def test_jobs_from_stdin(executable: str) -> None:
    """Job lines on stdin: defaults, overrides, and each rejected line."""
    lines = [
        '{"seed": 51}',
        '{"seed": 51, "sample": 1, "sampling_seed": 5}',
        "not json",
        '{"seed": 52, "rollout_id": "custom-a"}',
        '{"seed": 53, "rollout_id": "custom-a"}',
        "",
        '{"seed": 54, "samples": 2}',
        '{"seed": 55, "sampling_seed": null}',
        '{"seed": 56, "rollout_id": "' + "x" * 70_000 + '"}',
    ]
    with Stub(play_one_turn) as server:
        completed = subprocess.run(
            worker_command(
                executable,
                server,
                *("--jobs", "-", "--parallel", "2", "--rollout-prefix", PREFIX),
                *("--sampling-seed", "11", "--timeout-seconds", "15"),
            ),
            input="\n".join(lines) + "\n",
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        requests = list(server.requests)

    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 6, f"exit {completed.returncode}\n{output}")
    records = parse_output(completed.stdout, output)
    episodes, errors, batch = split_stream_records(records, output)

    # Line numbers count every line, the skipped blank one included.
    check(
        errors
        == [
            {"type": "job_error", "line": 3, "error": "invalid JSON at byte 2"},
            {
                "type": "job_error",
                "line": 5,
                "error": 'rollout id "custom-a" was already used on line 4',
            },
            {"type": "job_error", "line": 7, "error": 'unknown key "samples"'},
            {
                "type": "job_error",
                "line": 9,
                "error": "line is longer than 65536 bytes",
            },
        ],
        f"job errors: {errors!r}",
    )
    # A job line without a sampling seed (or with null) uses --sampling-seed.
    expected = {
        f"{PREFIX}/51/0": (51, 0, 11),
        f"{PREFIX}/51/1": (51, 1, 5),
        "custom-a": (52, 0, 11),
        f"{PREFIX}/55/0": (55, 0, 11),
    }
    check(episodes.keys() == expected.keys(), f"rollout ids: {sorted(episodes)}")
    for rollout, (seed, sample, sampling_seed) in expected.items():
        record = episodes[rollout]
        fields = {"seed": seed, "sample": sample, "sampling_seed": sampling_seed}
        check(subset(record, fields) == fields, f"{rollout}: {record!r}")
        check(record["reward"]["valid"] is True, f"{rollout}: {record!r}")
    for request in requests:
        check(request.rollout in expected, f"rollout header: {request!r}")
        seed, _, sampling_seed = expected[request.rollout]
        check(request.seed == str(seed), f"seed header: {request!r}")
        check(request.body.get("seed") == sampling_seed, f"{request.body!r}")
    check(len(requests) == 8, f"expected two requests per job, got {len(requests)}")

    expected_batch = {
        "status": "completed",
        "jobs": 4,
        "episodes": 4,
        "valid": 4,
        "invalid": 0,
        "not_started": 0,
        "job_errors": 4,
        "error": None,
        "exit_code": 6,
    }
    check(subset(batch, expected_batch) == expected_batch, f"batch: {batch!r}")
    for number in (3, 5, 7, 9):
        line = f"job line {number} rejected"
        check(line in completed.stderr, f"missing {line!r} on stderr\n{output}")


def start_stdin_worker(
    executable: str, server: StubServer, *extra: str
) -> subprocess.Popen[str]:
    return subprocess.Popen(
        worker_command(executable, server, "--jobs", "-", *extra),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def send_job(process: subprocess.Popen[str], job: dict[str, Any]) -> None:
    process.stdin.write(json.dumps(job) + "\n")
    process.stdin.flush()


def read_record(process: subprocess.Popen[str]) -> dict[str, Any]:
    line = process.stdout.readline()
    check(line.endswith("\n"), f"stdout ended early: {line!r}")
    return json.loads(line)


def test_slow_producer(executable: str) -> None:
    """A long-lived worker: each job runs as soon as its line arrives."""
    with Stub(play_one_turn) as server:
        process = start_stdin_worker(executable, server, "--timeout-seconds", "15")
        watchdog = threading.Timer(45, process.kill)
        watchdog.start()
        try:
            # Each record comes back while stdin is still open, so the
            # worker neither waits for the end of input nor reads ahead.
            for seed in (61, 62):
                send_job(process, {"seed": seed})
                record = read_record(process)
                expected = {"type": "episode", "rollout_id": f"rollout/{seed}/0"}
                check(subset(record, expected) == expected, f"record: {record!r}")
                check(record["reward"]["valid"] is True, f"record: {record!r}")
                check(
                    set(server.counts)
                    == {f"rollout/{s}/0" for s in range(61, seed + 1)},
                    f"rollouts so far: {sorted(server.counts)}",
                )
            process.stdin.close()
            batch = read_record(process)
            stderr = process.stderr.read()
            process.wait(timeout=30)
        finally:
            watchdog.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()

    check(process.returncode == 0, f"exit {process.returncode}\nstderr={stderr}")
    expected_batch = {
        "type": "batch",
        "status": "completed",
        "jobs": 2,
        "episodes": 2,
        "valid": 2,
        "job_errors": 0,
        "exit_code": 0,
    }
    check(subset(batch, expected_batch) == expected_batch, f"batch: {batch!r}")


def test_signal_while_waiting_for_jobs(executable: str) -> None:
    """A signal ends a worker blocked on stdin that is still open."""
    with Stub(play_one_turn) as server:
        process = start_stdin_worker(executable, server)
        try:
            send_job(process, {"seed": 71})
            record = read_record(process)
            check(record.get("rollout_id") == "rollout/71/0", f"{record!r}")
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    output = f"stdout={stdout}\nstderr={stderr}"
    check(process.returncode == 130, f"exit {process.returncode}\n{output}")
    batch = json.loads(stdout)
    expected_batch = {
        "type": "batch",
        "status": "interrupted",
        "jobs": 1,
        "episodes": 1,
        "valid": 1,
        "not_started": 0,
        "exit_code": 130,
    }
    check(subset(batch, expected_batch) == expected_batch, f"batch: {batch!r}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: worker_integration_tests.py PIG_PEN_WORKER")
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    test_parallel_batch(executable)
    test_invalid_episodes_exit_6(executable)
    test_usage_errors_exit_2(executable)
    test_jobs_from_stdin(executable)
    test_slow_producer(executable)
    if os.name == "posix":
        test_signal_while_waiting_for_jobs(executable)
        test_closed_stdout_exits_1(executable)
        for signal_number in (signal.SIGINT, signal.SIGTERM):
            test_signal_cancels_in_flight(executable, signal_number)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
