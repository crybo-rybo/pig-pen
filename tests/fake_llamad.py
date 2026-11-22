"""Run pigpen_fake_llamad, the scripted fake daemon, for the CLI tests."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import time
from types import TracebackType
from typing import Any

# What the fake reports from GetModelInfo; the CLI prints and logs it.
MODEL_INFO = {
    "description": "fake pig 1B Q4_K - Medium",
    "n_ctx": 32768,
    "n_ctx_train": 131072,
    "n_params": 1000000000,
    "size_bytes": 700000000,
    "model_name": "Fake Pig 1B",
    "model_file": "fake-pig-1b-q4_k_m.gguf",
    "llamad_version": "0.2.0",
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

    def __init__(
        self,
        executable: str,
        replies: list[dict],
        model_info: dict[str, Any] | None = None,
        *,
        gate_model_info: bool = False,
    ) -> None:
        """Start the fake; model_info replaces fields of MODEL_INFO.

        model_info can also script how GetModelInfo answers: "reply_delay_ms",
        "status_code", and "status_message". With gate_model_info, each
        GetModelInfo call waits until open_model_info_gate().
        """
        self._directory = tempfile.TemporaryDirectory(prefix="pigpen-", dir="/tmp")
        root = pathlib.Path(self._directory.name)
        self.socket = str(root / "s.sock")
        self._requests = root / "requests.jsonl"
        self._model_info_calls = root / "model-info-calls.txt"
        # Once this file exists, a GetModelInfo call that waits on it answers.
        self.model_info_gate = root / "model-info-gate"
        script = root / "script.json"
        info = {**MODEL_INFO, **(model_info or {})}
        script.write_text(json.dumps({"model_info": info, "replies": replies}))
        self._process = subprocess.Popen(
            [
                executable,
                *("--socket", self.socket),
                *("--script", str(script)),
                *("--requests", str(self._requests)),
                *("--model-info-calls", str(self._model_info_calls)),
                *(
                    ("--model-info-gate", str(self.model_info_gate))
                    if gate_model_info
                    else ()
                ),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert self._process.stdout is not None
        ready = self._process.stdout.readline()
        if ready.strip() != "ready":
            self.kill()
            self._directory.cleanup()
            raise AssertionError(f"fake llamad did not start: {ready!r}")

    def kill(self) -> None:
        """Stop the fake at once with SIGKILL, as if the daemon crashed."""
        self._process.kill()
        self._process.communicate(timeout=10)

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

    def model_info_calls(self) -> int:
        """How many GetModelInfo calls the fake received."""
        if not self._model_info_calls.exists():
            return 0
        return len(self._model_info_calls.read_text().splitlines())

    def wait_for_model_info_calls(self, count: int, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.model_info_calls() >= count:
                return
            time.sleep(0.01)
        raise AssertionError(
            f"expected {count} GetModelInfo calls, got {self.model_info_calls()}"
        )

    def open_model_info_gate(self) -> None:
        """Let the GetModelInfo calls that wait on the gate answer."""
        self.model_info_gate.touch()

    # typing.Self needs Python 3.11; CMake accepts any Python 3.
    def __enter__(self) -> FakeLlamad:  # noqa: PYI034
        return self

    def __exit__(
        self,
        kind: type[BaseException] | None,
        value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        # A GetModelInfo call that still waits on the gate would hold up the
        # shutdown of the fake.
        self.open_model_info_gate()
        self._process.terminate()
        try:
            self._process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.communicate()
        self._directory.cleanup()
