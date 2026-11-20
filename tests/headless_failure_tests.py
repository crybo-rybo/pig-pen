"""Drive pig-pen-headless into the failure paths of a request that hangs.

Each scenario starts the fake llamad with a reply that never finishes, waits
until the CLI's first request arrives, and then:

- daemon-killed: kills the fake with SIGKILL. The CLI must exit 1 and write
  an error turn and a complete footer.
- timeout: lets --timeout-seconds 1 run out. The CLI must exit 3 and write a
  cancelled turn and a complete footer.

No model or external network is used.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any, NamedTuple

from fake_llamad import FakeLlamad, check

# The CLI's own wait for the cancellation is 15 s after a timeout; this bound
# only stops a CLI that never exits.
EXIT_WAIT_SECONDS = 30


class Outcome(NamedTuple):
    returncode: int
    stdout: str
    stderr: str
    records: list[dict[str, Any]]

    def output(self) -> str:
        return f"exit {self.returncode}\nstdout={self.stdout}\nstderr={self.stderr}"


def run_hanging_turn(
    executables: tuple[str, str], timeout_seconds: int, *, kill_daemon: bool
) -> Outcome:
    """Run one turn whose only request hangs; kill the daemon if asked."""
    headless, fake = executables
    replies = [{"text": ["Thinking"], "hang": True}]
    with (
        FakeLlamad(fake, replies) as daemon,
        tempfile.TemporaryDirectory(prefix="pigpen-failure-test-") as log_dir,
    ):
        process = subprocess.Popen(
            [
                headless,
                *("--socket", daemon.socket, "--turns", "1"),
                *("--timeout-seconds", str(timeout_seconds), "--log-dir", log_dir),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            try:
                daemon.wait_for_requests(1)
            except AssertionError as error:
                process.kill()
                stdout, stderr = process.communicate()
                raise AssertionError(
                    f"{error}\nexit {process.returncode}\n"
                    f"stdout={stdout}\nstderr={stderr}"
                ) from None
            if kill_daemon:
                daemon.kill()
            try:
                stdout, stderr = process.communicate(timeout=EXIT_WAIT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
                raise AssertionError(
                    f"the CLI did not exit within {EXIT_WAIT_SECONDS} s\n"
                    f"stdout={stdout}\nstderr={stderr}"
                ) from None
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

        logs = list(pathlib.Path(log_dir).glob("*.jsonl"))
        outcome = Outcome(process.returncode, stdout, stderr, [])
        check(
            len(logs) == 1,
            f"expected one JSONL log, found {logs!r}\n{outcome.output()}",
        )
        records = [json.loads(line) for line in logs[0].read_text().splitlines()]
        return outcome._replace(records=records)


def check_log(outcome: Outcome, status: str, finish_reason: str) -> dict[str, Any]:
    """Check the one turn and the footer; return the turn."""
    output = outcome.output()
    records = outcome.records
    check(
        len(records) >= 2 and records[-1].get("type") == "footer",
        f"log has no terminal footer: {records!r}\n{output}",
    )
    turns = [record for record in records if record.get("type") == "turn"]
    check(len(turns) == 1, f"expected one turn: {turns!r}\n{output}")
    turn = turns[0]
    check(
        turn.get("status") == status, f"expected status {status!r}: {turn!r}\n{output}"
    )
    check(turn.get("requests") == 1, f"the turn must count its request: {turn!r}")
    footer = records[-1]
    check(footer.get("complete") is True, f"footer is not complete: {footer!r}")
    check(
        footer.get("finish_reason") == finish_reason,
        f"expected finish_reason {finish_reason!r}: {footer!r}\n{output}",
    )
    return turn


def test_daemon_killed(executables: tuple[str, str]) -> None:
    outcome = run_hanging_turn(executables, 60, kill_daemon=True)
    output = outcome.output()
    check(outcome.returncode == 1, f"expected exit 1\n{output}")
    turn = check_log(outcome, status="error", finish_reason="error")
    # The status name and message come from gRPC and differ between versions.
    error = turn.get("error", "")
    check(error.startswith("llamad "), f"unexpected turn error: {turn!r}\n{output}")
    check(
        f"terminal error: {error}" in outcome.stderr,
        f"terminal error missing from stderr\n{output}",
    )


def test_timeout(executables: tuple[str, str]) -> None:
    outcome = run_hanging_turn(executables, 1, kill_daemon=False)
    output = outcome.output()
    check(outcome.returncode == 3, f"expected exit 3\n{output}")
    check_log(outcome, status="cancelled", finish_reason="stopped")
    check(
        "timeout after 1 seconds" in outcome.stderr,
        f"timeout diagnostic missing from stderr\n{output}",
    )


SCENARIOS = {"daemon-killed": test_daemon_killed, "timeout": test_timeout}


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[3] not in SCENARIOS:
        raise SystemExit(
            "usage: headless_failure_tests.py PIG_PEN_HEADLESS PIGPEN_FAKE_LLAMAD "
            + "|".join(SCENARIOS)
        )
    executables = (
        str(pathlib.Path(sys.argv[1]).resolve()),
        str(pathlib.Path(sys.argv[2]).resolve()),
    )
    SCENARIOS[sys.argv[3]](executables)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
