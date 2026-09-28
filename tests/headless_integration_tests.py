"""Drive pig-pen-headless end to end against a loopback OpenAI-compatible stub.

Covers what the C++ suite cannot: the real curl/HTTP path, argv -> Config
wiring, stdout, exit codes 0 and 5, and a JSONL log written to disk.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any

MODEL = "headless-integration-model"
SAMPLING_SEED = 42
VALID_CALL_ID = "call-move-east"
INVALID_CALL_ID = "call-move-up"
FINAL_TEXT = "Moved east."
INVALID_FINAL_TEXT = "The invalid move was rejected."
DIRECTION_SCHEMA = {
    "additionalProperties": False,
    "properties": {
        "direction": {
            "description": "Cardinal direction: north, south, east, or west",
            "enum": ["north", "south", "east", "west"],
            "type": "string",
        }
    },
    "required": ["direction"],
    "type": "object",
}
EAT_SCHEMA = {
    "additionalProperties": False,
    "properties": {},
    "required": [],
    "type": "object",
}
# The provider-visible contract, in registration order: (name, description,
# parameters) as the HTTP request must carry them.
EXPECTED_TOOLS = [
    ("move", "Move one cell north, south, east, or west.", DIRECTION_SCHEMA),
    ("look", "Scan every cell in one direction to the wall.", DIRECTION_SCHEMA),
    ("eat", "Eat the item on the current cell, if present.", EAT_SCHEMA),
]
# Scry's accounting for a turn with one requested call, valid or not.
SCRY_ONE_CALL = {
    "rounds": 1,
    "calls": 1,
    "rejected_calls": 0,
    "round_limit_reached": False,
    "unexecuted_calls": 0,
}


def call_tally(**counts: int) -> dict[str, int]:
    """A turn's `calls` record: every bucket, zero unless given."""
    buckets = ("executed", "invalid", "budget_refused", "host_refused")
    return {bucket: counts.get(bucket, 0) for bucket in buckets}


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sse(delta: dict[str, Any], finish_reason: str) -> bytes:
    """One streamed completion: a delta chunk, a finish chunk, usage, [DONE]."""
    choices = [
        [{"index": 0, "delta": delta}],
        [{"index": 0, "delta": {}, "finish_reason": finish_reason}],
        [],
    ]
    chunks = [
        {"id": "chatcmpl-stub", "object": "chat.completion.chunk", "choices": c}
        for c in choices
    ]
    chunks[-1]["usage"] = {"prompt_tokens": 20, "completion_tokens": 4}
    frames = [f"data: {json.dumps(chunk)}\n\n" for chunk in chunks]
    return ("".join(frames) + "data: [DONE]\n\n").encode()


def tool_call_stream(call_id: str, name: str, arguments: dict[str, Any]) -> bytes:
    call = {
        "index": 0,
        "id": call_id,
        "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments)},
    }
    return sse({"role": "assistant", "tool_calls": [call]}, finish_reason="tool_calls")


def text_stream(text: str) -> bytes:
    return sse({"role": "assistant", "content": text}, finish_reason="stop")


class StubServer(HTTPServer):
    def __init__(self, responses: list[bytes]) -> None:
        super().__init__(("127.0.0.1", 0), StubHandler)
        self.responses = responses
        self.requests: list[tuple[str, dict[str, Any]]] = []


class StubHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: StubServer

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        self.server.requests.append((self.path, json.loads(self.rfile.read(length))))
        index = len(self.server.requests) - 1
        if index >= len(self.server.responses):
            self.send_error(500, f"unexpected request {index + 1}")
            return
        body = self.server.responses[index]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def log_message(self, format: str, *args: object) -> None:
        pass


def run(
    executable: str, responses: list[bytes], *extra: str
) -> tuple[subprocess.CompletedProcess[str], list[dict[str, Any]], list, str]:
    """Run one single-turn episode; return (process, requests, records, url)."""
    server = StubServer(responses)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}/v1"
    try:
        with tempfile.TemporaryDirectory(prefix="pigpen-headless-") as log_dir:
            completed = subprocess.run(
                [
                    executable,
                    *("--base-url", base_url, "--model", MODEL, "--seed", "0"),
                    *("--turns", "1", "--max-tool-rounds", "2"),
                    *("--timeout-seconds", "15", "--log-dir", log_dir),
                    *extra,
                ],
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
            logs = list(pathlib.Path(log_dir).glob("*.jsonl"))
            check(len(logs) == 1, f"expected one JSONL log, found {logs!r}")
            records = [json.loads(line) for line in logs[0].read_text().splitlines()]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    for path, _ in server.requests:
        check(path == "/v1/chat/completions", f"unexpected endpoint: {path}")
    return completed, [body for _, body in server.requests], records, base_url


def subset(record: dict[str, Any], expected: dict[str, Any]) -> dict[str, Any]:
    return {key: record.get(key) for key in expected}


def check_first_request(body: dict[str, Any]) -> None:
    check(body.get("model") == MODEL, f"wrong model: {body.get('model')!r}")
    check(body.get("stream") is True, "request did not enable streaming")
    tools = body.get("tools")
    check(isinstance(tools, list), f"tools is not an array: {tools!r}")
    # A list comparison also pins cardinality and rejects duplicate tools.
    sent = [
        (
            tool.get("type"),
            tool.get("function", {}).get("name"),
            tool.get("function", {}).get("description"),
            tool.get("function", {}).get("parameters"),
        )
        for tool in tools
    ]
    expected = [("function", *tool) for tool in EXPECTED_TOOLS]
    check(sent == expected, f"unexpected provider-visible tools: {tools!r}")


def only_tool_message(body: dict[str, Any]) -> dict[str, Any]:
    messages = [m for m in body["messages"] if m.get("role") == "tool"]
    check(len(messages) == 1, f"expected one tool message: {messages!r}")
    return messages[0]


def test_valid_move(executable: str) -> None:
    completed, requests, records, base_url = run(
        executable,
        [
            tool_call_stream(VALID_CALL_ID, "move", {"direction": "east"}),
            text_stream(FINAL_TEXT),
        ],
        "--sampling-seed",
        str(SAMPLING_SEED),
    )
    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 0, f"exit {completed.returncode}\n{output}")
    check(len(requests) == 2, f"expected two requests, got {len(requests)}")
    check_first_request(requests[0])
    check(
        all(body.get("seed") == SAMPLING_SEED for body in requests),
        "sampling seed was not sent on every request",
    )

    result = {
        "item_here": None,
        "ok": True,
        "position": {"x": 6, "y": 5},
        "reason": None,
    }
    message = only_tool_message(requests[1])
    check(message.get("tool_call_id") == VALID_CALL_ID, f"lost call id: {message!r}")
    check(json.loads(message["content"]) == result, f"tool result: {message!r}")

    canonical = json.dumps(result, separators=(",", ":"), sort_keys=True)
    activity = (
        'tool[turn=1,tick=1] move args={"direction":"east"} '
        f"result={canonical} position=(5,5)->(6,5)"
    )
    for line in (
        activity,
        FINAL_TEXT,
        "summary finish_reason=turn_budget",
    ):
        check(line in completed.stdout, f"missing {line!r} in stdout\n{output}")

    header, *body, footer = records
    expected_header = {
        "type": "header",
        "model": MODEL,
        "base_url": base_url,
        "sampling_seed": SAMPLING_SEED,
    }
    check(subset(header, expected_header) == expected_header, f"header: {header!r}")
    tools = [r for r in body if r["type"] == "tool"]
    turns = [r for r in body if r["type"] == "turn"]
    check(len(tools) + len(turns) == len(body), f"unexpected records: {body!r}")
    expected_tool = {
        "type": "tool",
        "turn": 1,
        "tick": 1,
        "tool": "move",
        "scry_turn_id": 1,
        "call_id": VALID_CALL_ID,
        "round": 1,
        "index": 0,
        "args": {"direction": "east"},
        "result": result,
        "before": {"x": 5, "y": 5},
        "after": {"x": 6, "y": 5},
        "action_executed": True,
        "result_dispatched": True,
        "score_after": 0,
    }
    check(tools == [expected_tool], f"tool records: {tools!r}")
    expected_turn = {
        "assistant_text": FINAL_TEXT,
        "tool_calls": 1,
        "scry_tools": SCRY_ONE_CALL,
    }
    check([subset(t, expected_turn) for t in turns] == [expected_turn], f"{turns!r}")
    tallies = [t.get("calls") for t in turns]
    check(tallies == [call_tally(executed=1)], f"call tally: {tallies!r}")
    expected_footer = {
        "type": "footer",
        "complete": True,
        "finish_reason": "turn_budget",
        "tool_call_counts": {"eat": 0, "look": 0, "move": 1},
    }
    check(subset(footer, expected_footer) == expected_footer, f"footer: {footer!r}")


def test_schema_rejection_exits_5(executable: str) -> None:
    completed, requests, records, _ = run(
        executable,
        [
            tool_call_stream(INVALID_CALL_ID, "move", {"direction": "up"}),
            text_stream(INVALID_FINAL_TEXT),
        ],
    )
    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 5, f"exit {completed.returncode}\n{output}")
    check(len(requests) == 2, f"expected two requests, got {len(requests)}")
    check_first_request(requests[0])
    check(all("seed" not in body for body in requests), "unset seed was sent")

    history = requests[1]["messages"]
    assistant_calls = [
        call
        for m in history
        if m.get("role") == "assistant"
        for call in m.get("tool_calls") or []
    ]
    check(
        [
            (c.get("id"), c["function"]["name"], json.loads(c["function"]["arguments"]))
            for c in assistant_calls
        ]
        == [(INVALID_CALL_ID, "move", {"direction": "up"})],
        f"rejected call missing from assistant history: {assistant_calls!r}",
    )
    message = only_tool_message(requests[1])
    check(message.get("tool_call_id") == INVALID_CALL_ID, f"lost call id: {message!r}")
    check(
        json.loads(message["content"])
        == {
            "error": "$.direction is not a declared enumerator; "
            "must be one of: north, south, east, west"
        },
        f"unexpected decode error: {message!r}",
    )

    check("tool[turn=" not in completed.stdout, f"world activity\n{output}")
    check(
        "summary finish_reason=turn_budget turns_used=1 turn_budget=1 score=0 "
        "tool_calls=0" in completed.stdout,
        f"unexpected summary\n{output}",
    )
    check("validation error:" in completed.stderr, f"no exit-5 diagnostic\n{output}")

    header, *turns, footer = records
    check(header.get("sampling_seed") is None, f"header: {header!r}")
    expected_turn = {
        "status": "completed",
        "assistant_text": INVALID_FINAL_TEXT,
        "tool_calls": 0,
        "zero_tool_turn": True,
        "scry_tools": SCRY_ONE_CALL,
    }
    check([subset(t, expected_turn) for t in turns] == [expected_turn], f"{turns!r}")
    tallies = [t.get("calls") for t in turns]
    check(tallies == [call_tally(invalid=1)], f"call tally: {tallies!r}")
    expected_footer = {
        "type": "footer",
        "complete": True,
        "finish_reason": "turn_budget",
        "final_score": 0,
        "tool_call_counts": {"eat": 0, "look": 0, "move": 0},
    }
    check(subset(footer, expected_footer) == expected_footer, f"footer: {footer!r}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: headless_integration_tests.py PIG_PEN_HEADLESS")
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    test_valid_move(executable)
    test_schema_rejection_exits_5(executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
