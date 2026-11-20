"""Drive pig-pen-headless into the failure paths of a daemon that hangs.

The first three scenarios start the fake llamad with a reply that never
finishes, wait until the CLI's first request arrives, and then:

- daemon-killed: kills the fake with SIGKILL. The CLI must exit 1 and write
  an error turn and a complete footer.
- timeout: lets --timeout-seconds 1 run out. The CLI must exit 3 and write a
  cancelled turn and a complete footer.
- stall: the reply is silent, and --stall-seconds 1 stops it. The CLI must
  exit 1 with the stall message well before --timeout-seconds, and write an
  error turn and a complete footer.

The other scenarios are about the handshake (GetModelInfo):

- connect-timeout: the fake does not answer, and --timeout-seconds 1 also
  limits the handshake. The CLI must exit 1 within about two seconds, not
  after the default handshake deadline of five seconds, and write no log.
- handshake-sigint / handshake-sigterm: the signal arrives while the
  handshake waits, then the handshake fails. The CLI must report the startup
  error and exit 130 or 143, not 1.

No model or external network is used.
"""

from __future__ import annotations

import json
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any, NamedTuple

from fake_llamad import FakeLlamad, check

# The CLI's own wait for the cancellation is 15 s after a timeout; this bound
# only stops a CLI that never exits.
EXIT_WAIT_SECONDS = 30
STALL_MESSAGE = "llamad sent nothing for 1 second, so Pig Pen stopped the request"


class Outcome(NamedTuple):
    returncode: int
    stdout: str
    stderr: str
    records: list[dict[str, Any]]
    # Seconds from the start of the CLI to its exit.
    elapsed: float = 0.0

    def output(self) -> str:
        return f"exit {self.returncode}\nstdout={self.stdout}\nstderr={self.stderr}"


def run_hanging_turn(
    executables: tuple[str, str],
    timeout_seconds: int,
    *,
    kill_daemon: bool,
    extra_arguments: tuple[str, ...] = (),
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
                *extra_arguments,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        started = time.monotonic()
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
            elapsed = time.monotonic() - started
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

        logs = list(pathlib.Path(log_dir).glob("*.jsonl"))
        outcome = Outcome(process.returncode, stdout, stderr, [], elapsed)
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


def test_stall(executables: tuple[str, str]) -> None:
    outcome = run_hanging_turn(
        executables, 30, kill_daemon=False, extra_arguments=("--stall-seconds", "1")
    )
    output = outcome.output()
    check(outcome.returncode == 1, f"expected exit 1\n{output}")
    turn = check_log(outcome, status="error", finish_reason="error")
    check(turn.get("error") == STALL_MESSAGE, f"unexpected turn error: {turn!r}")
    check(
        f"terminal error: {STALL_MESSAGE}" in outcome.stderr,
        f"stall message missing from stderr\n{output}",
    )
    # The stall stops the request after about one second; the 30 s episode
    # timeout must not be what ends it.
    check(outcome.elapsed < 15, f"the stall took {outcome.elapsed:.1f} s\n{output}")


def run_handshake(
    executables: tuple[str, str],
    model_info: dict[str, Any],
    arguments: tuple[str, ...],
    *,
    send_signal: int | None = None,
) -> Outcome:
    """Run the CLI against a fake whose GetModelInfo is scripted.

    With send_signal, the GetModelInfo call waits on the gate of the fake. The
    test sends the signal while the call waits, and then opens the gate, so
    the signal always arrives during the handshake.
    """
    headless, fake = executables
    with (
        FakeLlamad(
            fake, [], model_info, gate_model_info=send_signal is not None
        ) as daemon,
        tempfile.TemporaryDirectory(prefix="pigpen-failure-test-") as log_dir,
    ):
        started = time.monotonic()
        process = subprocess.Popen(
            [
                headless,
                *("--socket", daemon.socket, "--turns", "1", "--log-dir", log_dir),
                *arguments,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            daemon.wait_for_model_info_calls(1)
            if send_signal is not None:
                process.send_signal(send_signal)
                daemon.open_model_info_gate()
            stdout, stderr = process.communicate(timeout=EXIT_WAIT_SECONDS)
            elapsed = time.monotonic() - started
        except (AssertionError, subprocess.TimeoutExpired) as error:
            process.kill()
            stdout, stderr = process.communicate()
            raise AssertionError(
                f"{error}\nexit {process.returncode}\nstdout={stdout}\nstderr={stderr}"
            ) from None
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        outcome = Outcome(process.returncode, stdout, stderr, [], elapsed)
        check(daemon.requests() == [], f"the CLI sent a Chat request\n{outcome}")
        logs = list(pathlib.Path(log_dir).glob("*.jsonl"))
        check(logs == [], f"a failed handshake wrote a log: {logs!r}")
        return outcome._replace(stderr=stderr.replace(daemon.socket, "SOCKET"))


def test_connect_timeout(executables: tuple[str, str]) -> None:
    # The fake would answer after 30 s; the default handshake deadline is 5 s.
    outcome = run_handshake(
        executables, {"reply_delay_ms": 30000}, ("--timeout-seconds", "1")
    )
    output = outcome.output()
    check(
        outcome.elapsed < 4,
        f"the handshake took {outcome.elapsed:.1f} s, not about 1 s\n{output}",
    )
    check(outcome.returncode == 1, f"expected exit 1\n{output}")
    check(
        "startup error: could not reach llamad at SOCKET: it did not answer "
        "within 1 second" in outcome.stderr,
        f"startup error missing from stderr\n{output}",
    )


def test_handshake_signal(executables: tuple[str, str], name: str) -> None:
    signal_number = int(getattr(signal, name))
    outcome = run_handshake(
        executables,
        {"status_code": 13, "status_message": "the model is loading"},
        ("--timeout-seconds", "60"),
        send_signal=signal_number,
    )
    output = outcome.output()
    check(
        outcome.returncode == 128 + signal_number,
        f"expected exit {128 + signal_number}\n{output}",
    )
    check(
        "startup error: llamad at SOCKET did not report its model (llamad "
        "INTERNAL: the model is loading)" in outcome.stderr,
        f"startup error missing from stderr\n{output}",
    )


SCENARIOS = {
    "daemon-killed": test_daemon_killed,
    "timeout": test_timeout,
    "stall": test_stall,
    "connect-timeout": test_connect_timeout,
    "handshake-sigint": lambda executables: test_handshake_signal(
        executables, "SIGINT"
    ),
    "handshake-sigterm": lambda executables: test_handshake_signal(
        executables, "SIGTERM"
    ),
}


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
