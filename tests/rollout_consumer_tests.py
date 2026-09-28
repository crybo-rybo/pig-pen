"""Check examples/rollout_consumer.py, the reference trainer-side join.

Canned worker records and a canned request log cover every drop reason, the
arrival order of a trajectory, and group advantages (plain and normalised,
grouped by rollout-id prefix and seed, with the seed-only fallback).
A fake worker command covers the spawning form. Given the worker binary,
the consumer also joins a real pig-pen-worker batch against the requests a
threaded loopback stub logged, as a trainer's proxy would.
"""

from __future__ import annotations

import json
import math
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

from headless_integration_tests import check
from worker_integration_tests import Stub, play_one_turn, worker_command


def episode(
    rollout_id: str,
    reward: float | None,
    reason: str | None = None,
    seed: int | None = None,
) -> str:
    """A record; the seed and sample come from a `<prefix>/<seed>/<sample>` id
    unless the seed is given."""
    parts = rollout_id.split("/")
    record = {
        "type": "episode",
        "rollout_id": rollout_id,
        "seed": int(parts[-2]) if seed is None else seed,
        "sample": int(parts[-1]) if seed is None else 0,
        "reward": {
            "valid": reason is None,
            "invalid_reason": reason,
            "total": reward,
        },
    }
    return json.dumps(record)


RECORDS = [
    episode("r/1/0", 1.0),
    json.dumps({"type": "job_error", "line": 4, "error": 'unknown key "x"'}),
    episode("r/1/1", 3.0),
    episode("r/1/2", None, "timeout"),
    episode("r/2/0", 2.0),
    episode("r/3/0", 4.0),
    episode("r/1/3", 5.0),
    json.dumps({"type": "batch", "status": "completed", "exit_code": 6}),
]


def request(rollout_id: str, turn: int) -> str:
    return json.dumps(
        {
            "rollout_id": rollout_id,
            "request": {"messages": [{"role": "user", "content": f"turn {turn}"}]},
            "completion": {"text": f"{rollout_id} says {turn}", "logprobs": [-0.5]},
        }
    )


# Interleaved across rollouts, as parallel episodes arrive. r/2/0 has no
# requests (the proxy lost them); other/9/0 has no worker record.
REQUESTS = [
    request("r/1/0", 1),
    request("r/1/1", 1),
    request("other/9/0", 1),
    request("r/1/0", 2),
    request("r/1/2", 1),
    request("r/3/0", 1),
    request("r/1/1", 2),
    request("r/1/3", 1),
    "",
]


def run_consumer(
    consumer: str, *arguments: str
) -> tuple[list[dict[str, Any]], str, int]:
    completed = subprocess.run(
        [sys.executable, consumer, *arguments],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    output = [json.loads(line) for line in completed.stdout.splitlines()]
    return output, completed.stderr, completed.returncode


def by_rollout(output: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {line["rollout_id"]: line for line in output}


def test_canned_join(consumer: str, directory: pathlib.Path) -> None:
    records = directory / "records.jsonl"
    requests = directory / "requests.jsonl"
    records.write_text("\n".join(RECORDS) + "\n")
    requests.write_text("\n".join(REQUESTS) + "\n")

    output, stderr, code = run_consumer(
        consumer, "--requests", str(requests), "--records", str(records)
    )
    check(code == 0, f"exit {code}\nstderr={stderr}")
    rollouts = by_rollout(output)
    check(
        rollouts.keys() == {"r/1/0", "r/1/1", "r/1/3", "r/3/0"},
        f"kept: {sorted(rollouts)}",
    )
    # Seed 1 keeps rewards 1, 3, and 5 (the timed-out sample is dropped,
    # not counted as zero): mean 3.
    expected = {
        "r/1/0": (1.0, -2.0, 3),
        "r/1/1": (3.0, 0.0, 3),
        "r/1/3": (5.0, 2.0, 3),
        "r/3/0": (4.0, 0.0, 1),
    }
    for rollout_id, (reward, advantage, size) in expected.items():
        line = rollouts[rollout_id]
        check(line["reward"] == reward, f"{rollout_id}: {line!r}")
        check(math.isclose(line["advantage"], advantage), f"{rollout_id}: {line!r}")
        check(line["group_size"] == size, f"{rollout_id}: {line!r}")
    # A trajectory is its rollout's requests, in arrival order, untouched.
    trajectory = rollouts["r/1/0"]["trajectory"]
    check(
        [step["completion"]["text"] for step in trajectory]
        == ["r/1/0 says 1", "r/1/0 says 2"],
        f"trajectory: {trajectory!r}",
    )
    check(trajectory[0]["request"]["messages"][0]["content"] == "turn 1", "request")
    check(rollouts["r/1/1"]["seed"] == 1 and rollouts["r/1/1"]["sample"] == 1, "ids")
    check(rollouts["r/1/1"]["group"] == "r/1", f"group: {rollouts['r/1/1']!r}")

    for line in (
        "kept 4 rollouts",
        "dropped 1: invalid reward (timeout): r/1/2",
        "dropped 1: no logged requests: r/2/0",
        "dropped 1: no worker record: other/9/0",
        'job line 4 rejected: unknown key "x"',
        "batch completed, exit 6",
    ):
        check(line in stderr, f"missing {line!r} in\n{stderr}")

    # Normalised: seed 1's rewards have population std sqrt(8/3).
    output, stderr, code = run_consumer(
        consumer,
        *("--requests", str(requests), "--records", str(records), "--normalize"),
    )
    check(code == 0, f"exit {code}\nstderr={stderr}")
    std = math.sqrt(8 / 3)
    advantage = by_rollout(output)["r/1/3"]["advantage"]
    check(math.isclose(advantage, 2.0 / std, rel_tol=1e-5), f"normalised {advantage}")
    check(by_rollout(output)["r/3/0"]["advantage"] == 0.0, "single-rollout group")


def test_groups_by_prefix_and_seed(consumer: str, directory: pathlib.Path) -> None:
    """Training steps sharing a seed get separate baselines."""
    records = [
        episode("step17/1/0", 1.0),
        episode("step17/1/1", 3.0),
        episode("step18/1/0", 10.0),
        episode("step18/1/1", 20.0),
        # Not <prefix>/<seed>/<sample>: grouped by seed 1 alone.
        episode("trial-a", 7.0, seed=1),
        episode("x/trial-b", 9.0, seed=1),
        # Shaped like one, but for another seed than the record's.
        episode("run/2/0", 11.0, seed=1),
        # No prefix before the seed.
        episode("1/0", 3.0, seed=1),
    ]
    ids = [json.loads(line)["rollout_id"] for line in records]
    records_path = directory / "grouped-records.jsonl"
    requests_path = directory / "grouped-requests.jsonl"
    records_path.write_text("\n".join(records) + "\n")
    requests_path.write_text("\n".join(request(i, 1) for i in ids) + "\n")

    output, stderr, code = run_consumer(
        consumer, "--requests", str(requests_path), "--records", str(records_path)
    )
    check(code == 0, f"exit {code}\nstderr={stderr}")
    rollouts = by_rollout(output)
    expected = {
        "step17/1/0": ("step17/1", -1.0, 2),
        "step17/1/1": ("step17/1", 1.0, 2),
        "step18/1/0": ("step18/1", -5.0, 2),
        "step18/1/1": ("step18/1", 5.0, 2),
        # Rewards 7, 9, 11, 3: mean 7.5.
        "trial-a": ("1", -0.5, 4),
        "x/trial-b": ("1", 1.5, 4),
        "run/2/0": ("1", 3.5, 4),
        "1/0": ("1", -4.5, 4),
    }
    check(rollouts.keys() == expected.keys(), f"kept: {sorted(rollouts)}")
    for rollout_id, (group, advantage, size) in expected.items():
        line = rollouts[rollout_id]
        check(line["group"] == group, f"{rollout_id}: {line!r}")
        check(math.isclose(line["advantage"], advantage), f"{rollout_id}: {line!r}")
        check(line["group_size"] == size, f"{rollout_id}: {line!r}")


def test_spawned_worker(consumer: str, directory: pathlib.Path) -> None:
    """The consumer runs the command and reads its stdout."""
    requests = directory / "requests.jsonl"
    requests.write_text("\n".join(REQUESTS) + "\n")
    lines = "\n".join(RECORDS)
    fake = f"import sys; sys.stdout.write({lines!r} + '\\n'); sys.exit(6)"
    output, stderr, code = run_consumer(
        consumer, "--requests", str(requests), "--", sys.executable, "-c", fake
    )
    check(code == 0, f"exit {code}\nstderr={stderr}")
    check(len(output) == 4, f"kept {len(output)}\nstderr={stderr}")

    failing = "import sys; sys.exit(2)"
    output, stderr, code = run_consumer(
        consumer, "--requests", str(requests), "--", sys.executable, "-c", failing
    )
    check(code == 1 and output == [], f"exit {code}\nstderr={stderr}")
    check("worker exited 2" in stderr, f"stderr={stderr}")

    _, stderr, code = run_consumer(consumer, "--requests", str(requests))
    check(code == 2, f"exit {code}: neither --records nor a command")


def test_real_worker(consumer: str, worker: str, directory: pathlib.Path) -> None:
    """A real batch, joined against the requests the stub logged."""
    with Stub(play_one_turn) as server:
        completed = subprocess.run(
            worker_command(
                worker,
                server,
                *("--seeds", "81-82", "--samples", "3", "--parallel", "3"),
                *("--rollout-prefix", "live", "--timeout-seconds", "15"),
            ),
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        logged = list(server.requests)
    check(completed.returncode == 0, f"worker exit {completed.returncode}")

    records = directory / "live-records.jsonl"
    requests = directory / "live-requests.jsonl"
    records.write_text(completed.stdout)
    # What a trainer's proxy writes: the header, the body, what it sampled.
    requests.write_text(
        "".join(
            json.dumps(
                {
                    "rollout_id": entry.rollout,
                    "request": entry.body,
                    "completion": {"index": index},
                }
            )
            + "\n"
            for index, entry in enumerate(logged)
        )
    )
    output, stderr, code = run_consumer(
        consumer, "--requests", str(requests), "--records", str(records)
    )
    check(code == 0, f"exit {code}\nstderr={stderr}")
    rollouts = by_rollout(output)
    expected = {f"live/{seed}/{k}" for seed in (81, 82) for k in range(3)}
    check(rollouts.keys() == expected, f"kept: {sorted(rollouts)}\n{stderr}")
    worker_rewards = {
        record["rollout_id"]: record["reward"]["total"]
        for record in map(json.loads, completed.stdout.splitlines())
        if record["type"] == "episode"
    }
    for rollout_id, line in rollouts.items():
        check(line["reward"] == worker_rewards[rollout_id], f"{rollout_id} reward")
        # One episode turn: the tool-call request, then the text request.
        steps = line["trajectory"]
        check(len(steps) == 2, f"{rollout_id}: {len(steps)} steps")
        indices = [step["completion"]["index"] for step in steps]
        check(indices == sorted(indices), f"{rollout_id} out of order: {indices}")
        check(line["group_size"] == 3, f"{rollout_id}: {line!r}")
        check(line["group"] == f"live/{line['seed']}", f"{rollout_id}: {line!r}")
    for seed in (81, 82):
        group = [line for line in output if line["seed"] == seed]
        total = sum(line["advantage"] for line in group)
        check(math.isclose(total, 0.0, abs_tol=1e-9), f"seed {seed}: {total}")
    check("dropped" not in stderr, f"nothing should be dropped\n{stderr}")


def main() -> int:
    if len(sys.argv) not in (2, 3):
        raise SystemExit(
            "usage: rollout_consumer_tests.py ROLLOUT_CONSUMER [PIG_PEN_WORKER]"
        )
    consumer = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="pigpen-consumer-") as scratch:
        directory = pathlib.Path(scratch)
        test_canned_join(consumer, directory)
        test_groups_by_prefix_and_seed(consumer, directory)
        test_spawned_worker(consumer, directory)
        if len(sys.argv) == 3:
            worker = str(pathlib.Path(sys.argv[2]).resolve())
            test_real_worker(consumer, worker, directory)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
