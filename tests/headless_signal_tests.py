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
from typing import NoReturn


def fail(message: str, process: subprocess.Popen[str] | None = None) -> NoReturn:
    if process is not None and process.poll() is None:
        process.kill()
        process.communicate()
    raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 3:
        raise RuntimeError(
            "usage: headless_signal_tests.py PIGPEN_HEADLESS SIGINT|SIGTERM"
        )

    executable = pathlib.Path(sys.argv[1])
    signal_number = getattr(signal, sys.argv[2])
    expected_exit = 128 + signal_number

    listener = socket.create_server(("127.0.0.1", 0))
    listener.settimeout(10)
    port = listener.getsockname()[1]
    expected_model = "registry.example/pig-model:Q4_K_M"

    with tempfile.TemporaryDirectory(prefix="pigpen-signal-test-") as directory:
        command = [
            str(executable),
            "--base-url",
            f"http://127.0.0.1:{port}/v1",
            "--model",
            expected_model,
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
        except TimeoutError as error:
            process.kill()
            stdout, stderr = process.communicate()
            fail(
                f"headless process never connected: {error}\nstdout={stdout}\n"
                f"stderr={stderr}"
            )

        # Read far enough to prove that --model reached the server byte-for-byte,
        # then intentionally never send an HTTP response. This leaves a real
        # scry turn in flight without relying on a model server.
        with connection:
            connection.settimeout(10)
            request = b""
            expected_json_string = json.dumps(expected_model).encode()
            try:
                while expected_json_string not in request:
                    chunk = connection.recv(4096)
                    if not chunk:
                        break
                    request += chunk
            except TimeoutError as error:
                fail(
                    f"timed out reading model request: {error}; request={request!r}",
                    process,
                )
            if expected_json_string not in request:
                fail(
                    f"exact model identifier missing from request: {request!r}",
                    process,
                )

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
            fail(f"log changed the model identifier: {records[0]}")
        footer = records[-1]
        if footer.get("complete") is not True:
            fail(f"signal footer is not complete: {footer}")
        if footer.get("finish_reason") != "stopped":
            fail(f"unexpected signal finish reason: {footer}")
        reward = footer.get("reward") or {}
        if (
            reward.get("valid") is not False
            or reward.get("invalid_reason") != "stopped"
            or reward.get("total") is not None
        ):
            fail(f"a stopped episode must carry an invalid reward: {footer}")
        if " reward=invalid\n" not in stdout:
            fail(f"summary line does not mark the reward invalid: {stdout}")
        if "received signal" not in stderr:
            fail(f"signal diagnostic missing from stderr: {stderr}")
        if f'model="{expected_model}"' not in stdout:
            fail(f"startup summary changed the model identifier: {stdout}")

    listener.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
