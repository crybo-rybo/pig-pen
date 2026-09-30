"""Join pig-pen-worker records with a model server's request log.

A reference for the trainer side of docs/training.md, using only the
standard library. The worker labels every request with X-Pigpen-Rollout and
writes one `episode` record per rollout; the trainer's inference server (or a
proxy in front of it) logs what it sampled, keyed by that header. This script
joins the two into (trajectory, reward) pairs, drops what a trainer must not
train on, and computes GRPO-style group advantages: each reward minus the
mean reward of its group. It emits nothing unless the worker's terminal
`batch` record reports a completed batch, and it drops every occurrence of a
rollout id claimed by more than one episode record, since the requests under
such an id cannot be attributed to one run.

A group is the rollouts that share a world seed *and* a rollout-id prefix.
An id shaped `<prefix>/<seed>/<sample>` (the worker's default, or a trainer's
own such as `step17/1003/1`) is grouped under `<prefix>/<seed>`, so a
long-lived `--jobs -` stream that names its ids by training step never mixes
steps in one baseline. Any other id falls back to its seed alone, grouped
with the other such ids of that seed.

The request log is JSONL, one line per request in arrival order:

    {"rollout_id": "run42/1003/2", "request": {...}, "completion": {...}}

`rollout_id` is the X-Pigpen-Rollout header. `request` (the chat request
body) and `completion` (whatever the server sampled: text, token ids,
log-probabilities) are carried through untouched.

Usage:

    rollout_consumer.py --requests REQUESTS.jsonl --records ROLLOUTS.jsonl
    rollout_consumer.py --requests REQUESTS.jsonl -- pig-pen-worker ARGS...

The second form runs the worker and reads its stdout; the request log is
read once the worker has exited, since the server writes it concurrently.
Output is JSONL on stdout, one line per kept rollout:

    {"rollout_id", "seed", "sample", "group", "reward", "advantage",
     "group_size", "trajectory": [{"request", "completion"}, ...]}

where `group` is `<prefix>/<seed>`, or the seed alone for the fallback.

and a summary of what was kept and dropped on stderr.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from collections import defaultdict
from collections.abc import Iterable
from dataclasses import dataclass, field
from typing import Any

# Added to the group's standard deviation by --normalize, so a group whose
# rewards are all equal gets advantage 0 rather than a division by zero.
STD_EPSILON = 1e-6

# Process exit codes a batch whose episodes all finished reports: 0 when
# every episode was valid, 6 when some episodes or job lines were invalid
# but the valid rollouts are still usable (worker_main.cpp).
ACCEPTABLE_BATCH_EXIT_CODES = (0, 6)


@dataclass
class Rollout:
    """One episode that can be trained on."""

    rollout_id: str
    seed: int
    sample: int
    reward: float
    trajectory: list[dict[str, Any]]
    advantage: float = 0.0
    group: str = ""
    group_size: int = 1


@dataclass
class Join:
    """The joined rollouts and why everything else was left out."""

    rollouts: list[Rollout] = field(default_factory=list)
    # Reason -> rollout ids.
    dropped: dict[str, list[str]] = field(default_factory=lambda: defaultdict(list))
    job_errors: list[dict[str, Any]] = field(default_factory=list)
    batch: dict[str, Any] | None = None


def read_jsonl(lines: Iterable[str]) -> Iterable[dict[str, Any]]:
    for line in lines:
        if line.strip():
            yield json.loads(line)


def group_requests(lines: Iterable[str]) -> dict[str, list[dict[str, Any]]]:
    """Each rollout's logged requests, in arrival order."""
    steps: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for entry in read_jsonl(lines):
        steps[entry["rollout_id"]].append(
            {"request": entry.get("request"), "completion": entry.get("completion")}
        )
    return steps


def join(records: Iterable[str], requests: Iterable[str]) -> Join:
    """Pair every valid episode record with its logged requests.

    A rollout id used by more than one episode record is ambiguous: the
    requests under it cannot be split back across the runs, so every
    occurrence is dropped rather than joined against the combined log.
    """
    steps = group_requests(requests)
    result = Join()
    episodes: list[dict[str, Any]] = []
    counts: dict[str, int] = defaultdict(int)
    for record in read_jsonl(records):
        kind = record.get("type")
        if kind == "job_error":
            result.job_errors.append(record)
            continue
        if kind == "batch":
            result.batch = record
            continue
        if kind != "episode":
            continue
        counts[record["rollout_id"]] += 1
        episodes.append(record)
    ambiguous = {rollout_id for rollout_id, used in counts.items() if used > 1}
    seen: set[str] = set()
    for record in episodes:
        rollout_id = record["rollout_id"]
        if rollout_id in ambiguous:
            result.dropped["duplicate rollout id"].append(rollout_id)
            continue
        seen.add(rollout_id)
        reward = record["reward"]
        # An invalid reward is absent, not zero: never train on it.
        if not reward["valid"]:
            result.dropped[f"invalid reward ({reward['invalid_reason']})"].append(
                rollout_id
            )
            continue
        trajectory = steps.get(rollout_id)
        if not trajectory:
            result.dropped["no logged requests"].append(rollout_id)
            continue
        result.rollouts.append(
            Rollout(
                rollout_id=rollout_id,
                seed=record["seed"],
                sample=record["sample"],
                reward=reward["total"],
                trajectory=trajectory,
            )
        )
    for rollout_id in steps.keys() - seen:
        reason = (
            "duplicate rollout id" if rollout_id in ambiguous else "no worker record"
        )
        result.dropped[reason].append(rollout_id)
    return result


def group_key(rollout_id: str, seed: int) -> str:
    """`<prefix>/<seed>` for an id `<prefix>/<seed>/<sample>`, else the seed.

    The id must end in the record's own seed and a whole-number sample, with
    a non-empty prefix before them; anything else (a custom id such as
    `trial-a`) falls back to the seed alone.
    """
    prefix, _, rest = rollout_id.rpartition("/")
    prefix, _, id_seed = prefix.rpartition("/")
    if prefix and id_seed == str(seed) and rest.isdigit():
        return f"{prefix}/{seed}"
    return str(seed)


def add_advantages(rollouts: list[Rollout], normalize: bool = False) -> None:
    """Advantage = reward - mean reward of the rollout's group.

    Samples of one seed play the identical world, so the group mean is the
    baseline; the rollout-id prefix keeps separate runs or training steps
    apart. With `normalize`, the difference is also divided by the group's
    standard deviation, as GRPO does.
    """
    groups: dict[str, list[Rollout]] = defaultdict(list)
    for rollout in rollouts:
        rollout.group = group_key(rollout.rollout_id, rollout.seed)
        groups[rollout.group].append(rollout)
    for group in groups.values():
        rewards = [rollout.reward for rollout in group]
        mean = statistics.fmean(rewards)
        scale = statistics.pstdev(rewards) + STD_EPSILON if normalize else 1.0
        for rollout in group:
            rollout.advantage = (rollout.reward - mean) / scale
            rollout.group_size = len(group)


def run_worker(command: list[str]) -> tuple[list[str], int]:
    """The worker's stdout lines and exit code; its stderr passes through."""
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as worker:
        lines = list(worker.stdout)
    return lines, worker.returncode


def batch_problem(result: Join) -> str | None:
    """Why the joined batch is not safe to train on, or None.

    A batch is usable only when the worker's terminal `batch` record says it
    completed with an acceptable exit code: 0 (all valid) or 6 (some episodes
    or job lines invalid, the valid rollouts still usable). Anything else —
    aborted, interrupted, or no batch record at all — means the run did not
    finish, and emitting pairs would train on a partial batch.
    """
    batch = result.batch
    if batch is None:
        return "no batch record: the worker did not finish"
    status = batch.get("status")
    exit_code = batch.get("exit_code")
    if status != "completed" or exit_code not in ACCEPTABLE_BATCH_EXIT_CODES:
        return f"batch {status}, exit {exit_code}: did not complete"
    return None


def summarize(result: Join, out: Any) -> None:
    print(f"kept {len(result.rollouts)} rollouts", file=out)
    for reason, ids in sorted(result.dropped.items()):
        print(f"dropped {len(ids)}: {reason}: {', '.join(sorted(ids))}", file=out)
    for error in result.job_errors:
        print(f"job line {error['line']} rejected: {error['error']}", file=out)
    if result.batch is None:
        print("no batch record: the worker did not finish", file=out)
    else:
        batch = result.batch
        print(f"batch {batch['status']}, exit {batch['exit_code']}", file=out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Join pig-pen-worker records with a request log."
    )
    parser.add_argument("--requests", required=True, help="request log (JSONL)")
    parser.add_argument("--records", help="worker stdout saved to a file")
    parser.add_argument(
        "--normalize", action="store_true", help="divide by the group's std"
    )
    parser.add_argument("worker", nargs="*", help="-- then a worker command")
    args = parser.parse_args(argv)
    if bool(args.records) == bool(args.worker):
        parser.error("give either --records or -- WORKER COMMAND")

    if args.worker:
        records, code = run_worker(args.worker)
        # 6 still has valid rollouts; anything else means no usable batch.
        if code not in ACCEPTABLE_BATCH_EXIT_CODES:
            print(f"worker exited {code}", file=sys.stderr)
            return 1
    else:
        with open(args.records, encoding="utf-8") as file:
            records = file.readlines()
    with open(args.requests, encoding="utf-8") as file:
        result = join(records, file)

    problem = batch_problem(result)
    if problem is not None:
        # No usable batch: never emit training pairs from a partial run.
        print(problem, file=sys.stderr)
        return 1

    add_advantages(result.rollouts, args.normalize)
    for rollout in result.rollouts:
        line = {
            "rollout_id": rollout.rollout_id,
            "seed": rollout.seed,
            "sample": rollout.sample,
            "group": rollout.group,
            "reward": rollout.reward,
            "advantage": rollout.advantage,
            "group_size": rollout.group_size,
            "trajectory": rollout.trajectory,
        }
        print(json.dumps(line, sort_keys=True))
    summarize(result, sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
