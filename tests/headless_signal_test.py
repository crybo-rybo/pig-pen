#!/usr/bin/env python3
"""Verify signal-driven cancellation without a model or external network."""

from __future__ import annotations

import json
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading


def fail(message: str, process: subprocess.Popen[str] | None = None) -> None:
    if process is not None and process.poll() is None:
        process.kill()
        process.communicate()
    raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 3:
        raise RuntimeError(
            "usage: headless_signal_test.py PIGPEN_HEADLESS SIGINT|SIGTERM"
        )

    executable = pathlib.Path(sys.argv[1])
    signal_number = getattr(signal, sys.argv[2])
    expected_exit = 128 + signal_number

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(10)
    port = listener.getsockname()[1]

    with tempfile.TemporaryDirectory(prefix="pigpen-signal-test-") as directory:
        command = [
            str(executable),
            "--base-url",
            f"http://127.0.0.1:{port}/v1",
            "--model",
            "scripted-never-responds",
            "--turns",
            "1",
            "--timeout-seconds",
            "60",
            "--log-dir",
            directory,
        ]
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            connection, _ = listener.accept()
        except (TimeoutError, socket.timeout) as error:
            stdout, stderr = process.communicate(timeout=2)
            fail(
                f"headless process never connected: {error}\nstdout={stdout}\n"
                f"stderr={stderr}",
                process,
            )

        # The accepted socket intentionally never sends an HTTP response. This
        # leaves a real scry turn in flight without relying on a model server.
        with connection:
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
        footer = records[-1]
        if footer.get("complete") is not True:
            fail(f"signal footer is not complete: {footer}")
        if footer.get("finish_reason") != "stopped":
            fail(f"unexpected signal finish reason: {footer}")
        if "received signal" not in stderr:
            fail(f"signal diagnostic missing from stderr: {stderr}")

    listener.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
