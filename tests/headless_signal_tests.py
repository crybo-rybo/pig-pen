#!/usr/bin/env python3
"""Verify signal-driven cancellation without a model or external network."""

from __future__ import annotations

import json
import pathlib
import signal
import subprocess
import sys
import tempfile
from typing import NoReturn

from fake_llamad import MODEL_INFO, FakeLlamad


def fail(message: str, process: subprocess.Popen[str] | None = None) -> NoReturn:
    if process is not None and process.poll() is None:
        process.kill()
        process.communicate()
    raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        raise RuntimeError(
            "usage: headless_signal_tests.py PIGPEN_HEADLESS PIGPEN_FAKE_LLAMAD "
            "SIGINT|SIGTERM"
        )

    executable = pathlib.Path(sys.argv[1])
    signal_number = getattr(signal, sys.argv[3])
    expected_exit = 128 + signal_number
    expected_model = MODEL_INFO["description"]

    # The only reply streams some text and then never finishes, so a real
    # gRPC stream stays in flight until the CLI cancels it.
    replies = [{"text": ["Thinking"], "hang": True}]
    with (
        FakeLlamad(sys.argv[2], replies) as daemon,
        tempfile.TemporaryDirectory(prefix="pigpen-signal-test-") as directory,
    ):
        command = [
            str(executable),
            *("--socket", daemon.socket),
            *("--turns", "1", "--timeout-seconds", "60"),
            *("--log-dir", directory),
        ]
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            requests = daemon.wait_for_requests(1)
        except AssertionError as error:
            fail(f"headless process never sent a request: {error}", process)
        roles = [message["role"] for message in requests[0]["messages"]]
        if roles != ["system", "user"]:
            fail(f"unexpected first request: {requests[0]}", process)

        process.send_signal(signal_number)
        try:
            stdout, stderr = process.communicate(timeout=20)
        except subprocess.TimeoutExpired as error:
            fail(f"graceful signal shutdown timed out: {error}", process)

        if process.returncode != expected_exit:
            fail(
                f"expected exit {expected_exit}, got {process.returncode}\n"
                f"stdout={stdout}\nstderr={stderr}"
            )

        logs = list(pathlib.Path(directory).glob("*.jsonl"))
        if len(logs) != 1:
            fail(f"expected one JSONL log, found {logs}")
        records = [json.loads(line) for line in logs[0].read_text().splitlines()]
        if len(records) < 2 or records[-1].get("type") != "footer":
            fail(f"log has no terminal footer: {records}")
        if records[0].get("model") != expected_model:
            fail(f"log changed the model description: {records[0]}")
        if records[0].get("socket") != daemon.socket:
            fail(f"log has the wrong socket: {records[0]}")
        turns = [record for record in records if record.get("type") == "turn"]
        if len(turns) != 1 or turns[0].get("status") != "cancelled":
            fail(f"expected one cancelled turn: {turns}")
        if turns[0].get("requests") != 1:
            fail(f"cancelled turn does not count its one request: {turns[0]}")
        footer = records[-1]
        if footer.get("complete") is not True:
            fail(f"signal footer is not complete: {footer}")
        if footer.get("finish_reason") != "stopped":
            fail(f"unexpected signal finish reason: {footer}")
        if "received signal" not in stderr:
            fail(f"signal diagnostic missing from stderr: {stderr}")
        if f'model="{expected_model}"' not in stdout:
            fail(f"startup summary changed the model description: {stdout}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
