"""Run pigpen_fake_llamad, the scripted fake daemon, for the CLI tests."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import time
from types import TracebackType
from typing import Any, Self

# What the fake reports from GetModelInfo; the CLI prints and logs it.
MODEL_INFO = {
    "description": "fake pig 1B Q4_K - Medium",
    "n_ctx": 32768,
    "n_ctx_train": 131072,
    "n_params": 1000000000,
    "size_bytes": 700000000,
}


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def tool_call_reply(call_id: str, name: str, arguments: dict[str, Any]) -> dict:
    return {
        "finish": "tool_calls",
        "tool_calls": [
            {"id": call_id, "name": name, "arguments": json.dumps(arguments)}
        ],
        "prompt_tokens": 20,
        "completion_tokens": 4,
    }


def text_reply(text: str) -> dict:
    return {
        "text": [text],
        "finish": "eog",
        "prompt_tokens": 40,
        "completion_tokens": 3,
        "cached_prompt_tokens": 18,
    }


class FakeLlamad:
    """A pigpen_fake_llamad process on a socket in a short temporary directory.

    AF_UNIX paths are limited to about 107 bytes, so the socket never goes
    under the build tree.
    """

    def __init__(self, executable: str, replies: list[dict]) -> None:
        self._directory = tempfile.TemporaryDirectory(prefix="pigpen-", dir="/tmp")
        root = pathlib.Path(self._directory.name)
        self.socket = str(root / "s.sock")
        self._requests = root / "requests.jsonl"
        script = root / "script.json"
        script.write_text(json.dumps({"model_info": MODEL_INFO, "replies": replies}))
        self._process = subprocess.Popen(
            [
                executable,
                *("--socket", self.socket),
                *("--script", str(script)),
                *("--requests", str(self._requests)),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert self._process.stdout is not None
        ready = self._process.stdout.readline()
        check(ready.strip() == "ready", f"fake llamad did not start: {ready!r}")

    def requests(self) -> list[dict]:
        """Every Chat request the fake received, in order."""
        if not self._requests.exists():
            return []
        lines = self._requests.read_text().splitlines()
        return [json.loads(line) for line in lines if line]

    def wait_for_requests(self, count: int, timeout: float = 10.0) -> list[dict]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            requests = self.requests()
            if len(requests) >= count:
                return requests
            time.sleep(0.01)
        raise AssertionError(f"expected {count} requests, got {self.requests()!r}")

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        kind: type[BaseException] | None,
        value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._process.terminate()
        try:
            self._process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.communicate()
        self._directory.cleanup()
