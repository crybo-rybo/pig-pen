#!/usr/bin/env python3
"""Test bench/pigpen_bench.py without a model server or a real model.

A fake pig-pen-headless (a small Python script) parses the same flags, writes
a JSONL log like the real CLI, and exits with a code that each test selects.
HTTP checks use a stub server on loopback.

Usage: bench_tests.py [PIG_PEN_HEADLESS] [unittest options]

With a PIG_PEN_HEADLESS path, the contract test checks that its --help still
names every flag that the bench emits. Without one, that test is skipped.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "bench" / "pigpen_bench.py"
_spec = importlib.util.spec_from_file_location("pigpen_bench", SCRIPT)
assert _spec is not None and _spec.loader is not None
bench = importlib.util.module_from_spec(_spec)
sys.modules["pigpen_bench"] = bench
_spec.loader.exec_module(bench)

# Set from argv[1] by main(); ctest passes $<TARGET_FILE:pig-pen-headless>.
HEADLESS: str | None = None

FAKE_HEADLESS = r"""
import json, os, pathlib, signal, sys, time

VALUE_FLAGS = {
    "--base-url", "--model", "--seed", "--turns", "--max-tool-rounds",
    "--temperature", "--sampling-seed", "--timeout-seconds", "--log-dir",
    "--prompt-variant", "--input",
}
BOOL_FLAGS = {"--hidden-values", "--no-reward-feedback", "--opaque-look"}


def main():
    argv = sys.argv[1:]
    if argv == ["--help"]:
        print("Options:\n" + "\n".join(sorted(VALUE_FLAGS | BOOL_FLAGS)))
        return 0
    options, index = {}, 0
    while index < len(argv):
        name = argv[index]
        if name in BOOL_FLAGS:
            options[name] = True
            index += 1
        elif name in VALUE_FLAGS and index + 1 < len(argv):
            options[name] = argv[index + 1]
            index += 2
        else:
            print(f"option error: unknown option: {name}", file=sys.stderr)
            return 2
    state = pathlib.Path(os.environ["FAKE_STATE"])
    with open(state / "calls.jsonl", "a") as calls:
        calls.write(json.dumps(argv) + "\n")
    plan_path = state / "plan.json"
    plan = json.loads(plan_path.read_text()) if plan_path.exists() else {}
    scenario = options["--prompt-variant"].rsplit(":", 1)[-1]
    key = f"{options['--model']}/{scenario}/{options['--seed']}"
    episode = {**plan.get("default", {}), **plan.get("episodes", {}).get(key, {})}

    exit_code = episode.get("exit", 0)
    finish_reason = episode.get("finish_reason", "turn_budget")
    if episode.get("wait_for_signal"):
        received = []
        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, lambda signum, frame: received.append(signum))
        (state / "started").write_text("")
        deadline = time.monotonic() + 20
        while not received and time.monotonic() < deadline:
            time.sleep(0.01)
        exit_code = 128 + received[0] if received else 1
        finish_reason = "stopped"
    print(f"session model={options['--model']!r}")
    print("assistant[turn=1]: thinking", file=sys.stdout)
    if episode.get("stderr"):
        print(episode["stderr"], file=sys.stderr)
    if not episode.get("write_log", True):
        return exit_code

    turns = episode.get("turns") or [
        {"tool_calls": 2, "scry_tools": {"calls": 2}, "zero_tool_turn": False,
         "latency_ms": 1000, "output_tokens": 50},
        {"tool_calls": 2, "scry_tools": {"calls": 2}, "zero_tool_turn": False,
         "latency_ms": 3000, "output_tokens": 150},
    ]
    records = [{
        "type": "header", "model": options["--model"],
        "base_url": options["--base-url"], "seed": int(options["--seed"]),
        "prompt_variant": options["--prompt-variant"],
        "scenario": {
            "items": {"berry": 6, "apple": 3, "truffle": 1, "toadstool": 3},
            "turn_budget": int(options["--turns"]),
            "known_item_values": "--hidden-values" not in options,
            "reward_feedback": "--no-reward-feedback" not in options,
            "opaque_look": "--opaque-look" in options,
        },
    }]
    for number, turn in enumerate(turns, start=1):
        records.append({"type": "tool", "turn": number, "tool": "look"})
        records.append({"type": "turn", "turn": number, "status": "completed",
                        **turn})
    records.append({
        "type": "footer", "complete": True, "finish_reason": finish_reason,
        "final_score": episode.get("score", 5),
        "items_eaten": {"apple": 0, "berry": 0, "truffle": 0,
                        "toadstool": episode.get("toadstools", 0)},
        "turns_used": episode.get("turns_used", len(turns)),
    })
    log_dir = pathlib.Path(options["--log-dir"])
    log_dir.mkdir(parents=True, exist_ok=True)
    name = "".join(c if c.isalnum() or c in "-_." else "_"
                   for c in options["--model"])
    path = log_dir / f"20261022-120000-000-{name}-{options['--seed']}.jsonl"
    path.write_text("".join(json.dumps(r, sort_keys=True) + "\n" for r in records))
    print("summary finish_reason=" + finish_reason)
    return exit_code


sys.exit(main())
"""

SUITE = """\
name = "{name}"
description = "A tiny suite for tests."
seeds = [1, 2]
turns = 3
max_tool_rounds = 2
temperature = 0.5
sampling_seed = 7
timeout_seconds = 60

[[scenarios]]
name = "default"

[[scenarios]]
name = "blind"
hidden_values = true
no_reward_feedback = true
opaque_look = true
"""

README = """\
# pig-pen

Intro.

## Leaderboard

<!-- leaderboard:start -->
old content
<!-- leaderboard:end -->

## The pen
"""


def closed_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class StubServer(HTTPServer):
    """/v1/models and /api/show, like Ollama."""

    def __init__(self, models: list[str], details: dict[str, Any]) -> None:
        super().__init__(("127.0.0.1", 0), StubHandler)
        self.models = models
        self.details = details
        self.requests: list[tuple[str, str, dict[str, str], Any]] = []

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.server_port}/v1"


class StubHandler(BaseHTTPRequestHandler):
    server: StubServer

    def reply(self, body: Any, status: int = 200) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        self.server.requests.append(("GET", self.path, dict(self.headers), None))
        if self.path == "/v1/models":
            self.reply({"data": [{"id": m} for m in self.server.models]})
        else:
            self.reply({"error": "not found"}, 404)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length))
        self.server.requests.append(("POST", self.path, dict(self.headers), body))
        if self.path == "/api/show" and body.get("model") in self.server.details:
            self.reply({"details": self.server.details[body["model"]]})
        else:
            self.reply({"error": "model not found"}, 404)

    def log_message(self, format: str, *args: object) -> None:
        pass


@contextlib.contextmanager
def stub_server(models: list[str], details: dict[str, Any]):
    server = StubServer(models, details)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


class BenchTestCase(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="pigpen-bench-")
        self.addCleanup(directory.cleanup)
        self.tmp = pathlib.Path(directory.name)
        self.state = self.tmp / "state"
        self.state.mkdir()
        self.fake = self.tmp / "fake-headless"
        self.fake.write_text(f"#!{sys.executable}\n{FAKE_HEADLESS}")
        self.fake.chmod(0o755)
        self.suite = self.write_suite("tiny")
        self.runs = self.tmp / "runs"
        self.results = self.tmp / "results"
        self.readme = self.tmp / "README.md"
        self.readme.write_text(README)
        self.leaderboard = self.tmp / "leaderboard.md"
        self.closed_url = f"http://127.0.0.1:{closed_port()}/v1"
        environment = {"FAKE_STATE": str(self.state)}
        patcher = unittest.mock.patch.dict(os.environ, environment)
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop("PIGPEN_API_KEY", None)

    def write_suite(self, name: str, text: str | None = None) -> pathlib.Path:
        path = self.tmp / f"{name}.toml"
        path.write_text(text if text is not None else SUITE.format(name=name))
        return path

    def plan(self, default: dict | None = None, **episodes: dict) -> None:
        """Episode keys are "model/scenario/seed"; use plan(**{key: ...})."""
        plan = {"default": default or {}, "episodes": episodes}
        (self.state / "plan.json").write_text(json.dumps(plan))

    def calls(self) -> list[list[str]]:
        path = self.state / "calls.jsonl"
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text().splitlines()]

    def bench(self, *args: str) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = bench.main(list(args))
        return status, stdout.getvalue(), stderr.getvalue()

    def run_bench(self, *args: str, preflight: bool = False) -> tuple[int, str, str]:
        extra = [] if preflight else ["--no-preflight"]
        if "--base-url" not in args:
            extra += ["--base-url", self.closed_url]
        return self.bench(
            "run",
            *("--suite", str(self.suite)),
            *("--headless", str(self.fake)),
            *("--runs-dir", str(self.runs)),
            *extra,
            *args,
        )

    def outputs(self) -> list[str]:
        return [
            *("--results-dir", str(self.results)),
            *("--readme", str(self.readme)),
            *("--leaderboard", str(self.leaderboard)),
        ]

    def report(self, *args: str, suite: pathlib.Path | None = None):
        return self.bench(
            "report",
            *("--suite", str(suite or self.suite)),
            *("--runs-dir", str(self.runs)),
            *self.outputs(),
            *args,
        )

    def episode(self, model: str, scenario: str, seed: int) -> pathlib.Path:
        suite = bench.load_suite(str(self.suite))
        return (
            suite.runs_dir(self.runs)
            / bench.model_slug(model)
            / scenario
            / (f"seed-{seed}")
        )

    def sidecar(self, model: str, scenario: str, seed: int) -> dict | None:
        path = self.episode(model, scenario, seed) / "episode.json"
        return json.loads(path.read_text()) if path.exists() else None


class RunTests(BenchTestCase):
    def test_exact_argv_for_each_scenario(self) -> None:
        status, stdout, _ = self.run_bench("--model", "acme/pig:Q4")
        self.assertEqual(status, 0, stdout)
        base = [
            *("--base-url", self.closed_url, "--model", "acme/pig:Q4"),
        ]
        expected = []
        for scenario, flags in (
            ("default", []),
            ("blind", ["--hidden-values", "--no-reward-feedback", "--opaque-look"]),
        ):
            for seed in (1, 2):
                expected.append(
                    [
                        *base,
                        *("--seed", str(seed), "--turns", "3"),
                        *("--max-tool-rounds", "2", "--temperature", "0.5"),
                        *("--sampling-seed", "7", "--timeout-seconds", "60"),
                        *(
                            "--log-dir",
                            str(self.episode("acme/pig:Q4", scenario, seed)),
                        ),
                        *("--prompt-variant", f"bench:tiny:{scenario}"),
                        *flags,
                    ]
                )
        self.assertEqual(self.calls(), expected)
        self.assertIn("[1/4] acme/pig:Q4 default seed=1: score=5", stdout)
        self.assertIn("finish=turn_budget outcome=ok", stdout)

    def test_unset_sampling_seed_is_not_sent(self) -> None:
        text = SUITE.format(name="nosampling").replace("sampling_seed = 7\n", "")
        self.suite = self.write_suite("nosampling", text)
        status, _, _ = self.run_bench("--model", "m")
        self.assertEqual(status, 0)
        self.assertTrue(all("--sampling-seed" not in call for call in self.calls()))

    def test_sidecar_and_episode_files(self) -> None:
        self.run_bench("--model", "m")
        sidecar = self.sidecar("m", "default", 1)
        assert sidecar is not None
        suite = bench.load_suite(str(self.suite))
        self.assertEqual(sidecar["model"], "m")
        self.assertEqual(sidecar["scenario"], "default")
        self.assertEqual(sidecar["seed"], 1)
        self.assertEqual(sidecar["exit_code"], 0)
        self.assertEqual(sidecar["outcome"], "ok")
        self.assertEqual(
            sidecar["suite"], {"name": "tiny", "fingerprint": suite.fingerprint}
        )
        self.assertIsNone(sidecar["model_metadata"])
        self.assertTrue(sidecar["log_file"].endswith("-m-1.jsonl"))
        for key in ("started_at", "finished_at", "wall_seconds", "pigpen_commit"):
            self.assertIn(key, sidecar)
        directory = self.episode("m", "default", 1)
        self.assertIn("summary", (directory / "stdout.txt").read_text())

    def test_models_file_order_and_duplicates(self) -> None:
        models = self.tmp / "models.txt"
        models.write_text("# local models\nb\n\n  a  # comment\nb\n")
        self.assertEqual(
            bench.collect_models(
                [("model", "c"), ("file", str(models)), ("model", "a")]
            ),
            ["c", "b", "a"],
        )

    def test_resume_skips_recorded_episodes(self) -> None:
        self.run_bench("--model", "m")
        self.assertEqual(len(self.calls()), 4)
        status, stdout, _ = self.run_bench("--model", "m")
        self.assertEqual(status, 0)
        self.assertEqual(len(self.calls()), 4)
        self.assertIn("already_recorded=4", stdout)

    def test_outcomes_by_exit_code(self) -> None:
        self.plan(
            **{
                "m/default/1": {"exit": 5},
                "m/default/2": {"exit": 3, "finish_reason": "stopped"},
                "m/blind/1": {"exit": 1, "finish_reason": "error"},
                "m/blind/2": {"exit": 9},
            }
        )
        status, _, _ = self.run_bench("--model", "m", "--max-consecutive-errors", "5")
        self.assertEqual(status, 0)
        outcomes = [
            self.sidecar("m", scenario, seed)["outcome"]
            for scenario in ("default", "blind")
            for seed in (1, 2)
        ]
        self.assertEqual(outcomes, ["no_tool_calls", "timeout", "error", "error"])

    def test_retry_errors(self) -> None:
        self.plan(**{"m/default/2": {"exit": 1, "stderr": "terminal error: boom"}})
        _, _, stderr = self.run_bench("--model", "m")
        self.assertIn("terminal error: boom", stderr)
        self.assertEqual(self.sidecar("m", "default", 2)["outcome"], "error")
        self.plan()
        self.run_bench("--model", "m")
        self.assertEqual(len(self.calls()), 4, "errors are skipped on resume")
        self.run_bench("--model", "m", "--retry-errors")
        self.assertEqual(len(self.calls()), 5)
        self.assertEqual(self.calls()[-1][self.calls()[-1].index("--seed") + 1], "2")
        self.assertEqual(self.sidecar("m", "default", 2)["outcome"], "ok")

    def test_force_reruns_and_clears_the_episode_dir(self) -> None:
        self.run_bench("--model", "m")
        directory = self.episode("m", "default", 1)
        (directory / "stale.jsonl").write_text("{}\n")
        self.run_bench("--model", "m", "--force")
        self.assertEqual(len(self.calls()), 8)
        names = sorted(p.name for p in directory.iterdir())
        self.assertEqual(len([n for n in names if n.endswith(".jsonl")]), 1)
        self.assertEqual(
            [n for n in names if not n.endswith(".jsonl")],
            ["episode.json", "stderr.txt", "stdout.txt"],
        )

    def test_interrupted_child_stops_the_bench(self) -> None:
        for code in bench.INTERRUPT_EXITS:
            with self.subTest(code=code):
                (self.state / "calls.jsonl").unlink(missing_ok=True)
                self.plan(**{"m/default/2": {"exit": code, "finish_reason": "stopped"}})
                status, _, stderr = self.run_bench("--model", "m", "--force")
                self.assertEqual(status, code)
                self.assertIn("interrupted", stderr)
                self.assertEqual(len(self.calls()), 2, "the bench stops at once")
                self.assertIsNone(self.sidecar("m", "default", 2))
                self.assertIsNotNone(self.sidecar("m", "default", 1))
        self.plan()
        (self.state / "calls.jsonl").unlink()
        status, _, _ = self.run_bench("--model", "m")
        self.assertEqual(status, 0)
        self.assertEqual(len(self.calls()), 3, "resume reruns the interrupted one")

    @unittest.skipUnless(os.name == "posix", "needs POSIX signals")
    def test_signal_is_forwarded_and_nothing_is_recorded(self) -> None:
        for name in ("SIGINT", "SIGTERM"):
            with self.subTest(signal=name):
                for path in self.state.iterdir():
                    path.unlink()
                self.plan(**{"m/default/1": {"wait_for_signal": True}})
                process = subprocess.Popen(
                    [
                        sys.executable,
                        str(SCRIPT),
                        "run",
                        *("--suite", str(self.suite), "--headless", str(self.fake)),
                        *("--runs-dir", str(self.runs), "--model", "m"),
                        *("--base-url", self.closed_url, "--no-preflight", "--force"),
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                deadline = time.monotonic() + 15
                while not (self.state / "started").exists():
                    if time.monotonic() > deadline or process.poll() is not None:
                        process.kill()
                        self.fail(f"fake never started: {process.communicate()}")
                    time.sleep(0.01)
                process.send_signal(getattr(signal, name))
                _, stderr = process.communicate(timeout=15)
                self.assertEqual(
                    process.returncode, 128 + getattr(signal, name), stderr
                )
                self.assertEqual(len(self.calls()), 1)
                self.assertIsNone(self.sidecar("m", "default", 1))
                log = bench.find_log(self.episode("m", "default", 1))
                assert log is not None
                footer = bench.read_log(log)["footer"]
                self.assertEqual(footer["finish_reason"], "stopped")

    def test_exit_2_and_4_abort_the_bench(self) -> None:
        for code in (2, 4):
            with self.subTest(code=code):
                (self.state / "calls.jsonl").unlink(missing_ok=True)
                self.plan({"exit": code, "stderr": "option error: nope"})
                status, _, stderr = self.run_bench("--model", "m", "--model", "n")
                self.assertEqual(status, bench.EXIT_ABORTED)
                self.assertIn(f"(exit {code})", stderr)
                self.assertIn("option error: nope", stderr)
                self.assertEqual(len(self.calls()), 1)
                self.assertIsNone(self.sidecar("m", "default", 1))

    def test_consecutive_errors_skip_the_model(self) -> None:
        self.plan(
            **{
                f"bad/{s}/{n}": {"exit": 1}
                for s in ("default", "blind")
                for n in (1, 2)
            }
        )
        status, _, stderr = self.run_bench(
            "--model", "bad", "--model", "good", "--max-consecutive-errors", "2"
        )
        self.assertEqual(status, bench.EXIT_MODELS_SKIPPED)
        self.assertIn("2 consecutive errors for 'bad'", stderr)
        models = [call[call.index("--model") + 1] for call in self.calls()]
        self.assertEqual(models, ["bad", "bad", "good", "good", "good", "good"])

    def test_dry_run_prints_the_plan(self) -> None:
        self.run_bench("--model", "m")
        status, stdout, _ = self.run_bench("--model", "m", "--model", "n", "--dry-run")
        self.assertEqual(status, 0)
        self.assertEqual(len(self.calls()), 4, "a dry run starts nothing")
        self.assertIn("8 episode(s), 4 to run, 4 already recorded", stdout)
        self.assertIn("[1/8] skip m default seed=1 (recorded: ok)", stdout)
        self.assertIn("[5/8] run  " + str(self.fake), stdout)
        self.assertIn("--prompt-variant bench:tiny:default", stdout)

    def test_invalid_usage(self) -> None:
        status, _, stderr = self.run_bench()
        self.assertEqual(status, bench.EXIT_USAGE)
        self.assertIn("at least one --model", stderr)
        status, _, stderr = self.bench(
            "run", "--suite", str(self.suite), "--model", "m", "--no-preflight",
            "--headless", str(self.tmp / "missing"), "--runs-dir", str(self.runs),
        )  # fmt: skip
        self.assertEqual(status, bench.EXIT_USAGE)
        self.assertIn("not found", stderr)


class PreflightTests(BenchTestCase):
    def test_unreachable_server_aborts(self) -> None:
        status, _, stderr = self.run_bench("--model", "m", preflight=True)
        self.assertEqual(status, bench.EXIT_ABORTED)
        self.assertIn("preflight", stderr)
        self.assertEqual(self.calls(), [])

    def test_missing_model_is_skipped_and_metadata_recorded(self) -> None:
        details = {
            "parameter_size": "8.0B",
            "quantization_level": "Q4_K_M",
            "family": "llama",
            "format": "gguf",
        }
        with (
            stub_server(["present:latest", "other"], {"present": details}) as server,
            unittest.mock.patch.dict(os.environ, {"PIGPEN_API_KEY": "secret"}),
        ):
            status, _, stderr = self.run_bench(
                "--model", "absent", "--model", "present",
                "--base-url", server.base_url, preflight=True,
            )  # fmt: skip
        self.assertEqual(status, bench.EXIT_MODELS_SKIPPED)
        self.assertIn("'absent' is not listed", stderr)
        self.assertEqual({c[c.index("--model") + 1] for c in self.calls()}, {"present"})
        self.assertEqual(
            self.sidecar("present", "blind", 2)["model_metadata"],
            {
                "parameter_size": "8.0B",
                "quantization_level": "Q4_K_M",
                "family": "llama",
            },
        )
        gets = [r for r in server.requests if r[0] == "GET"]
        shows = [r for r in server.requests if r[1] == "/api/show"]
        self.assertEqual(len(gets), 1)
        self.assertEqual(gets[0][2].get("Authorization"), "Bearer secret")
        self.assertEqual([r[3] for r in shows], [{"model": "present"}])

    def test_metadata_failure_is_not_fatal(self) -> None:
        with stub_server(["m"], {}) as server:
            status, _, _ = self.run_bench(
                "--model", "m", "--base-url", server.base_url, preflight=True
            )
        self.assertEqual(status, 0)
        self.assertIsNone(self.sidecar("m", "default", 1)["model_metadata"])


def log_with(
    score: int,
    finish: str = "turn_budget",
    turns: list[dict] | None = None,
    toadstools: int = 0,
    turns_used: int = 2,
    items: dict | None = None,
) -> dict:
    return {
        "header": {"scenario": {"items": items or bench.DEFAULT_ITEM_COUNTS}},
        "footer": {
            "final_score": score,
            "finish_reason": finish,
            "items_eaten": {"toadstool": toadstools},
            "turns_used": turns_used,
        },
        "turns": turns or [],
        "tools": [],
    }


def turn(
    tool_calls: int,
    calls: int | None,
    latency: int | None = 100,
    tokens: int | None = 10,
) -> dict:
    return {
        "tool_calls": tool_calls,
        "scry_tools": None if calls is None else {"calls": calls},
        "zero_tool_turn": tool_calls == 0,
        "latency_ms": latency,
        "output_tokens": tokens,
    }


class AggregationTests(unittest.TestCase):
    def test_metrics_math(self) -> None:
        ok = {"outcome": "ok"}
        episodes = [
            (
                ok,
                log_with(
                    25, "objective_complete", [turn(3, 4, 100, 10), turn(0, 1, 300)], 1
                ),
            ),
            # An error keeps the score its footer recorded. Its cancelled turn
            # has no Scry statistics, so it is not part of the valid-call rate.
            (
                {"outcome": "error"},
                log_with(5, "error", [turn(2, None, None, None)], 2, 1),
            ),
            ({"outcome": "timeout"}, None),  # no usable log: counts as 0
            (None, None),  # never recorded: counts as 0
        ]
        metrics = bench.scenario_metrics(episodes)
        self.assertEqual(metrics["episodes_scheduled"], 4)
        self.assertEqual(metrics["episodes_recorded"], 2)
        self.assertTrue(metrics["incomplete"])
        self.assertEqual(metrics["outcomes"], {"error": 1, "ok": 1, "timeout": 1})
        self.assertEqual(metrics["max_score"], 25)
        self.assertEqual(metrics["mean_score"], 7.5)
        self.assertEqual(metrics["stdev_score"], round(11.9024, 4))
        self.assertEqual((metrics["min_score"], metrics["max_score_seen"]), (0, 25))
        self.assertEqual(metrics["percent_of_max"], 30.0)
        self.assertEqual(metrics["cleared_rate"], 0.25)
        self.assertEqual(metrics["mean_toadstools"], 0.75)
        self.assertEqual(metrics["mean_turns_used"], 1.5)
        self.assertEqual(metrics["valid_call_rate"], 0.6)
        self.assertEqual(metrics["zero_tool_turn_rate"], round(1 / 3, 4))
        self.assertEqual(metrics["errors_timeouts"], 2)
        self.assertEqual(metrics["median_latency_ms"], 200)
        self.assertEqual(metrics["mean_output_tokens"], 10)

    def test_complete_and_empty(self) -> None:
        metrics = bench.scenario_metrics([({"outcome": "ok"}, log_with(4))])
        self.assertFalse(metrics["incomplete"])
        self.assertEqual(metrics["stdev_score"], 0.0)
        self.assertIsNone(metrics["valid_call_rate"])
        self.assertIsNone(metrics["median_latency_ms"])
        empty = bench.scenario_metrics(
            [(None, None), ({"outcome": "interrupted"}, None)]
        )
        self.assertEqual(empty["episodes_recorded"], 0)
        self.assertEqual(empty["outcomes"], {})
        self.assertEqual(empty["mean_score"], 0)

    def test_max_score_comes_from_the_header_items(self) -> None:
        self.assertEqual(bench.max_score(None), 25)
        items = {"berry": 2, "truffle": 1, "toadstool": 3}
        self.assertEqual(bench.max_score({"scenario": {"items": items}}), 12)
        metrics = bench.scenario_metrics(
            [({"outcome": "ok"}, log_with(6, items=items))]
        )
        self.assertEqual(metrics["percent_of_max"], 50.0)

    def test_ranking_tie_breaks(self) -> None:
        def m(score: float, cleared: float, turns: float | None) -> dict:
            return {
                "scenarios": {
                    "default": {
                        "mean_score": score,
                        "cleared_rate": cleared,
                        "mean_turns_used": turns,
                    }
                }
            }

        models = {
            "z-low": m(3, 1.0, 1),
            "y-fewer-turns": m(10, 0.5, 8),
            "x-more-turns": m(10, 0.5, 12),
            "w-cleared": m(10, 0.6, 20),
            "b-same": m(10, 0.5, 12),
            "a-no-turns": m(10, 0.5, None),
            "top": m(11, 0.0, 20),
        }
        order = [name for name, _, _ in bench.ranked(models, "default")]
        self.assertEqual(
            order,
            [
                "top",
                "w-cleared",
                "y-fewer-turns",
                "b-same",
                "x-more-turns",
                "a-no-turns",
                "z-low",
            ],
        )


class ReportTests(BenchTestCase):
    def results_json(self) -> dict:
        return json.loads((self.results / "tiny.json").read_text())

    def test_report_aggregates_and_renders(self) -> None:
        self.plan(
            {"score": 10},
            **{
                "m/default/2": {"score": 25, "finish_reason": "objective_complete"},
                "m/blind/1": {"write_log": False, "exit": 1},
            },
        )
        self.run_bench("--model", "m")
        status, stdout, stderr = self.report()
        self.assertEqual(status, 0, stderr)
        results = self.results_json()
        suite = bench.load_suite(str(self.suite))
        self.assertEqual(results["suite"]["fingerprint"], suite.fingerprint)
        self.assertEqual(results["suite"]["settings"], suite.settings())
        self.assertEqual(results["max_score"], 25)
        self.assertIn("generated_at", results)
        self.assertIn("pigpen_commits", results)
        default = results["models"]["m"]["scenarios"]["default"]
        self.assertEqual(default["mean_score"], 17.5)
        self.assertEqual(default["cleared_rate"], 0.5)
        blind = results["models"]["m"]["scenarios"]["blind"]
        self.assertEqual(blind["mean_score"], 5.0)
        self.assertTrue(blind["incomplete"])
        self.assertEqual(blind["errors_timeouts"], 1)
        self.assertIn("default #1 m: mean 17.5", stdout)
        page = self.leaderboard.read_text()
        self.assertIn("## Suite `tiny`", page)
        self.assertIn("### Scenario `blind`", page)
        self.assertIn("1/2\\*", page)

    def test_merge_keeps_old_models_and_prune_drops_them(self) -> None:
        self.run_bench("--model", "old")
        self.report()
        for path in sorted(self.runs.rglob("*"), reverse=True):
            path.rmdir() if path.is_dir() else path.unlink()
        self.plan({"score": 20})
        self.run_bench("--model", "new")
        status, stdout, _ = self.report()
        self.assertEqual(status, 0)
        self.assertIn("1 kept from the earlier results", stdout)
        self.assertEqual(sorted(self.results_json()["models"]), ["new", "old"])
        self.plan({"score": 1})
        self.run_bench("--model", "new", "--force")
        self.report()
        new = self.results_json()["models"]["new"]["scenarios"]["default"]
        self.assertEqual(new["mean_score"], 1.0, "runs replace the earlier entry")
        status, _, _ = self.report("--prune")
        self.assertEqual(status, 0)
        self.assertEqual(sorted(self.results_json()["models"]), ["new"])

    def test_changed_suite_needs_prune(self) -> None:
        self.run_bench("--model", "m")
        self.report()
        self.write_suite(
            "tiny", SUITE.format(name="tiny").replace("turns = 3", "turns = 4")
        )
        self.run_bench("--model", "m")
        status, _, stderr = self.report()
        self.assertEqual(status, bench.EXIT_FAILURE)
        self.assertIn("--prune", stderr)
        status, _, _ = self.report("--prune")
        self.assertEqual(status, 0)
        self.assertEqual(self.results_json()["suite"]["settings"]["turns"], 4)

    def test_report_without_runs_fails(self) -> None:
        status, _, stderr = self.report()
        self.assertEqual(status, bench.EXIT_FAILURE)
        self.assertIn("no recorded episodes", stderr)


class RenderTests(BenchTestCase):
    def standard_results(self, models: dict[str, Any]) -> None:
        suite = bench.load_suite("standard")
        metrics = bench.scenario_metrics([({"outcome": "ok"}, log_with(10))] * 10)
        document = {
            "schema_version": bench.SCHEMA_VERSION,
            "suite": {
                "name": "standard",
                "description": suite.description,
                "fingerprint": suite.fingerprint,
                "settings": suite.settings(),
            },
            "generated_at": "2026-10-22T12:00:00Z",
            "max_score": 25,
            "pigpen_commits": ["abc1234"],
            "models": {
                name: {
                    "metadata": metadata,
                    "pigpen_commits": ["abc1234"],
                    "last_run": "2026-10-22T11:00:00Z",
                    "scenarios": {"default": metrics},
                }
                for name, metadata in models.items()
            },
        }
        self.results.mkdir(exist_ok=True)
        (self.results / "standard.json").write_text(json.dumps(document))

    def render(self) -> tuple[int, str, str]:
        return self.bench("render", *self.outputs())

    def test_placeholder_without_results(self) -> None:
        status, _, _ = self.render()
        self.assertEqual(status, 0)
        text = self.readme.read_text()
        self.assertIn("No results yet — see [docs/benchmark.md]", text)
        self.assertNotIn("old content", text)
        self.assertTrue(text.endswith("## The pen\n"))

    def test_render_is_idempotent_and_escapes_model_names(self) -> None:
        self.standard_results(
            {"pipe|model*": {"parameter_size": "7B", "quantization_level": "Q4_K_M"}}
        )
        self.render()
        first = (self.readme.read_text(), self.leaderboard.read_text())
        status, stdout, _ = self.render()
        self.assertEqual(status, 0)
        self.assertEqual((self.readme.read_text(), self.leaderboard.read_text()), first)
        self.assertEqual(stdout.count("unchanged"), 2)
        rows = [line for line in first[0].splitlines() if line.startswith("| 1 |")]
        self.assertEqual(len(rows), 1)
        self.assertIn("pipe\\|model\\*", rows[0])
        self.assertIn("Q4\\_K\\_M", rows[0])
        unescaped = rows[0].replace("\\|", "")
        self.assertEqual(unescaped.count("|"), 11, "ten columns")
        self.assertIn("10.0 ± 0.0", rows[0])
        self.assertIn("Seeds 1–10, 20 turns", first[0])
        self.assertIn("`abc1234`", first[0])
        self.assertIn("[Leaderboard](docs/leaderboard.md)", first[0])

    def test_marker_errors(self) -> None:
        for text, message in (
            ("# no markers\n", "found 0 and 0"),
            (README + bench.README_START + "\n", "found 2 and 1"),
            (f"{bench.README_END}\n{bench.README_START}\n", "comes before"),
        ):
            with self.subTest(message=message):
                self.readme.write_text(text)
                status, _, stderr = self.render()
                self.assertEqual(status, bench.EXIT_FAILURE)
                self.assertIn(message, stderr)
                self.assertEqual(self.readme.read_text(), text)
                self.assertFalse(self.leaderboard.exists())

    def test_committed_readme_is_rendered(self) -> None:
        """The committed README and leaderboard page match a fresh render."""
        readme = (ROOT / "README.md").read_text()
        documents = bench.load_all_results(bench.RESULTS_DIR)
        self.assertEqual(
            bench.splice(readme, bench.readme_section(documents), "README.md"), readme
        )
        self.assertEqual(
            (ROOT / "docs" / "leaderboard.md").read_text(),
            bench.leaderboard_page(documents),
        )


class SuiteTests(BenchTestCase):
    def test_committed_suites(self) -> None:
        standard = bench.load_suite("standard")
        self.assertEqual(standard.seeds, tuple(range(1, 11)))
        self.assertEqual(
            (standard.turns, standard.max_tool_rounds, standard.temperature),
            (20, 8, 0.0),
        )
        self.assertEqual((standard.sampling_seed, standard.timeout_seconds), (0, 900))
        self.assertEqual([s.name for s in standard.scenarios], ["default"])
        quick = bench.load_suite("quick")
        self.assertEqual((quick.seeds, quick.turns), ((1, 2, 3), 6))
        scenarios = bench.load_suite("scenarios")
        self.assertEqual(
            [(s.name, s.flags()) for s in scenarios.scenarios],
            [
                ("default", []),
                ("hidden-values", ["--hidden-values"]),
                ("no-reward-feedback", ["--no-reward-feedback"]),
                ("opaque-look", ["--opaque-look"]),
                ("blind", ["--hidden-values", "--no-reward-feedback", "--opaque-look"]),
            ],
        )
        self.assertEqual(scenarios.seeds, (1, 2, 3, 4, 5))

    def test_validation_errors(self) -> None:
        base = SUITE.format(name="bad")
        cases = [
            (base + "extra = 1\n", "unknown key(s): extra"),
            (base.replace('description = "A tiny suite for tests."\n', ""), "missing"),
            (base.replace("turns = 3", "turns = 0"), "turns must be in the range"),
            (base.replace("max_tool_rounds = 2", "max_tool_rounds = 65"), "1..64"),
            (base.replace("temperature = 0.5", "temperature = 2.5"), "temperature"),
            (base.replace("temperature = 0.5", 'temperature = "hot"'), "a number"),
            (
                base.replace("sampling_seed = 7", "sampling_seed = 4294967296"),
                "0..4294",
            ),
            (
                base.replace("timeout_seconds = 60", "timeout_seconds = 86401"),
                "1..86400",
            ),
            (base.replace("seeds = [1, 2]", "seeds = [1, 1]"), "must not repeat"),
            (base.replace("seeds = [1, 2]", "seeds = []"), "non-empty"),
            (base.replace("seeds = [1, 2]", "seeds = [-1]"), "each seed"),
            (base.replace("turns = 3", "turns = true"), "must be an integer"),
            (base.replace("opaque_look = true", "opaque_look = 1"), "true or false"),
            (
                base.replace("opaque_look = true", "opaque = true"),
                "unknown key(s): opaque",
            ),
            (base.replace('name = "blind"', 'name = "default"'), "must not repeat"),
            (base.replace('name = "blind"', 'name = "Blind!"'), "lowercase"),
            (
                base.replace('name = "bad"', 'name = "other"'),
                "must match the file name",
            ),
            ("name = [", "not valid TOML"),
        ]
        for text, message in cases:
            with self.subTest(message=message):
                path = self.write_suite("bad", text)
                with self.assertRaises(bench.BenchError) as raised:
                    bench.load_suite(str(path))
                self.assertIn(message, str(raised.exception))
                self.assertEqual(raised.exception.status, bench.EXIT_USAGE)
        with self.assertRaisesRegex(bench.BenchError, "available: quick"):
            bench.load_suite("no-such-suite")

    def test_fingerprint_isolation(self) -> None:
        suite = bench.load_suite(str(self.suite))
        renamed = SUITE.format(name="tiny").replace("A tiny", "Another tiny")
        self.write_suite("tiny", renamed)
        self.assertEqual(
            bench.load_suite(str(self.suite)).fingerprint, suite.fingerprint
        )
        self.assertEqual(
            bench.load_suite(
                str(self.write_suite("tiny", renamed.replace("0.5", "0.50")))
            ).fingerprint,
            suite.fingerprint,
        )
        self.run_bench("--model", "m")
        self.write_suite("tiny", renamed.replace("turns = 3", "turns = 5"))
        changed = bench.load_suite(str(self.suite))
        self.assertNotEqual(changed.fingerprint, suite.fingerprint)
        status, stdout, _ = self.run_bench("--model", "m")
        self.assertEqual(status, 0)
        self.assertIn("4 to run, 0 already recorded", stdout)
        self.assertEqual(len(self.calls()), 8)
        self.assertEqual(
            sorted(p.name for p in self.runs.iterdir()),
            sorted([f"tiny-{suite.fingerprint}", f"tiny-{changed.fingerprint}"]),
        )

    def test_model_slugs_never_collide(self) -> None:
        self.assertEqual(bench.model_slug("llama3.1-8b"), "llama3.1-8b")
        names = ["a:b", "a_b", "A_b", "a/b", "", ".", "..", "_"]
        slugs = [bench.model_slug(name) for name in names]
        self.assertEqual(len(set(slugs)), len(names))
        for slug in slugs:
            self.assertRegex(slug, r"\A[a-z0-9_][a-z0-9._-]*\Z")
        self.assertTrue(bench.model_slug("a:b").startswith("a_b-"))


class ContractTests(unittest.TestCase):
    def test_help_names_every_emitted_flag(self) -> None:
        if not HEADLESS:
            self.skipTest("no pig-pen-headless path given")
        completed = subprocess.run(
            [HEADLESS, "--help"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        for flag in bench.EMITTED_FLAGS:
            self.assertRegex(completed.stdout, rf"(?m)^\s*{flag}\b", flag)


def main() -> None:
    global HEADLESS
    argv = sys.argv[:]
    if len(argv) > 1 and not argv[1].startswith("-"):
        HEADLESS = str(pathlib.Path(argv.pop(1)).resolve())
    unittest.main(argv=argv)


if __name__ == "__main__":
    main()
