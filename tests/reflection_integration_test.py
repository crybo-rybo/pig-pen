"""Exercise Pig Pen's reflected tools through the public provider boundary."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any

EXPECTED_MODEL = "reflection-integration-model"
TOOL_CALL_ID = "call-reflected-move"
FINAL_TEXT = "Moved east through the reflected contract."
INVALID_TOOL_CALL_ID = "call-invalid-reflected-move"
INVALID_FINAL_TEXT = "Invalid move was rejected by reflected decoding."

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


def check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sse_stream(*chunks: dict[str, Any]) -> bytes:
    frames = [
        "data: " + json.dumps(chunk, separators=(",", ":")) + "\n\n" for chunk in chunks
    ]
    frames.append("data: [DONE]\n\n")
    return "".join(frames).encode()


TOOL_STREAM = sse_stream(
    {
        "id": "chatcmpl-reflection-tool",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {
                    "role": "assistant",
                    "tool_calls": [
                        {
                            "index": 0,
                            "id": TOOL_CALL_ID,
                            "type": "function",
                            "function": {
                                "name": "move",
                                "arguments": json.dumps(
                                    {"direction": "east"}, separators=(",", ":")
                                ),
                            },
                        }
                    ],
                },
                "finish_reason": None,
            }
        ],
    },
    {
        "id": "chatcmpl-reflection-tool",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "tool_calls",
            }
        ],
    },
    {
        "id": "chatcmpl-reflection-tool",
        "object": "chat.completion.chunk",
        "choices": [],
        "usage": {
            "prompt_tokens": 20,
            "completion_tokens": 4,
            "total_tokens": 24,
        },
    },
)

FINAL_STREAM = sse_stream(
    {
        "id": "chatcmpl-reflection-final",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {"role": "assistant", "content": FINAL_TEXT},
                "finish_reason": None,
            }
        ],
    },
    {
        "id": "chatcmpl-reflection-final",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "stop",
            }
        ],
    },
    {
        "id": "chatcmpl-reflection-final",
        "object": "chat.completion.chunk",
        "choices": [],
        "usage": {
            "prompt_tokens": 35,
            "completion_tokens": 7,
            "total_tokens": 42,
        },
    },
)

INVALID_TOOL_STREAM = sse_stream(
    {
        "id": "chatcmpl-invalid-reflection-tool",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {
                    "role": "assistant",
                    "tool_calls": [
                        {
                            "index": 0,
                            "id": INVALID_TOOL_CALL_ID,
                            "type": "function",
                            "function": {
                                "name": "move",
                                "arguments": json.dumps(
                                    {"direction": "up"}, separators=(",", ":")
                                ),
                            },
                        }
                    ],
                },
                "finish_reason": None,
            }
        ],
    },
    {
        "id": "chatcmpl-invalid-reflection-tool",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "tool_calls",
            }
        ],
    },
    {
        "id": "chatcmpl-invalid-reflection-tool",
        "object": "chat.completion.chunk",
        "choices": [],
        "usage": {
            "prompt_tokens": 20,
            "completion_tokens": 4,
            "total_tokens": 24,
        },
    },
)

INVALID_FINAL_STREAM = sse_stream(
    {
        "id": "chatcmpl-invalid-reflection-final",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {
                    "role": "assistant",
                    "content": INVALID_FINAL_TEXT,
                },
                "finish_reason": None,
            }
        ],
    },
    {
        "id": "chatcmpl-invalid-reflection-final",
        "object": "chat.completion.chunk",
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "stop",
            }
        ],
    },
    {
        "id": "chatcmpl-invalid-reflection-final",
        "object": "chat.completion.chunk",
        "choices": [],
        "usage": {
            "prompt_tokens": 35,
            "completion_tokens": 7,
            "total_tokens": 42,
        },
    },
)


@dataclass(frozen=True)
class ProviderRequest:
    path: str
    body: dict[str, Any]


@dataclass(frozen=True)
class ProviderResponse:
    body: bytes
    request_id: str


@dataclass(frozen=True)
class ScenarioResult:
    completed: subprocess.CompletedProcess[str]
    requests: list[ProviderRequest]
    records: list[dict[str, Any]]
    base_url: str


class ReflectionServer(HTTPServer):
    requests: list[ProviderRequest]
    errors: list[str]

    def __init__(self, responses: list[ProviderResponse]) -> None:
        super().__init__(("127.0.0.1", 0), ReflectionRequestHandler)
        self.responses = responses
        self.requests = []
        self.errors = []
        self._lock = threading.Lock()

    def record(self, request: ProviderRequest) -> int:
        with self._lock:
            index = len(self.requests)
            self.requests.append(request)
            return index

    def record_error(self, message: str) -> None:
        with self._lock:
            self.errors.append(message)


class ReflectionRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:
        server = self.server
        assert isinstance(server, ReflectionServer)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length))
            check(isinstance(body, dict), "provider request body is not an object")
            request_index = server.record(ProviderRequest(self.path, body))
            if request_index >= len(server.responses):
                raise RuntimeError(
                    f"unexpected provider request number {request_index + 1}"
                )
            response = server.responses[request_index]
            self._send_sse(response.body, response.request_id)
        # The handler thread must relay every fixture failure to the test thread.
        except Exception as error:  # noqa: BLE001
            server.record_error(f"{type(error).__name__}: {error}")
            self._send_error_response()

    def _send_sse(self, body: bytes, request_id: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("x-request-id", request_id)
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def _send_error_response(self) -> None:
        body = b"provider fixture error\n"
        self.send_response(500)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def log_message(self, format: str, *args: object) -> None:
        del format, args


def run_scenario(
    executable: pathlib.Path,
    responses: list[ProviderResponse],
    name: str,
) -> ScenarioResult:
    server = ReflectionServer(responses)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}/v1"

    try:
        with tempfile.TemporaryDirectory(
            prefix=f"pigpen-reflection-{name}-"
        ) as directory:
            log_directory = pathlib.Path(directory)
            command = [
                str(executable),
                "--base-url",
                base_url,
                "--model",
                EXPECTED_MODEL,
                "--seed",
                "0",
                "--turns",
                "1",
                "--max-tool-rounds",
                "2",
                "--timeout-seconds",
                "15",
                "--log-dir",
                str(log_directory),
                "--prompt-variant",
                f"reflection-integration-{name}",
            ]
            try:
                completed = subprocess.run(
                    command,
                    capture_output=True,
                    text=True,
                    timeout=20,
                    check=False,
                )
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    f"headless {name} integration run timed out: {error}"
                ) from error

            logs = list(log_directory.glob("*.jsonl"))
            check(len(logs) == 1, f"expected one {name} JSONL log, found {logs!r}")
            records = [json.loads(line) for line in logs[0].read_text().splitlines()]
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)

    check(not server.errors, f"{name} provider fixture failed: {server.errors!r}")
    return ScenarioResult(
        completed=completed,
        requests=list(server.requests),
        records=records,
        base_url=base_url,
    )


def assert_reflected_tools(request: ProviderRequest) -> None:
    check(
        request.path == "/v1/chat/completions",
        f"unexpected provider endpoint: {request.path}",
    )
    body = request.body
    check(body.get("model") == EXPECTED_MODEL, f"wrong model: {body.get('model')}")
    check(body.get("stream") is True, "provider request did not enable SSE")

    tools = body.get("tools")
    check(isinstance(tools, list), f"tools is not an array: {tools!r}")
    check(len(tools) == 3, f"expected exactly three reflected tools: {tools!r}")
    functions = {
        tool["function"]["name"]: tool
        for tool in tools
        if isinstance(tool, dict)
        and isinstance(tool.get("function"), dict)
        and isinstance(tool["function"].get("name"), str)
    }
    check(
        set(functions) == {"move", "look", "eat"},
        f"unexpected reflected tool names: {set(functions)}",
    )

    expected = {
        "move": (
            "Move one cell north, south, east, or west.",
            DIRECTION_SCHEMA,
        ),
        "look": (
            "Scan every cell in one direction to the wall.",
            DIRECTION_SCHEMA,
        ),
        "eat": (
            "Eat the item on the current cell, if present.",
            EAT_SCHEMA,
        ),
    }
    for name, (description, schema) in expected.items():
        tool = functions[name]
        function = tool["function"]
        check(tool.get("type") == "function", f"{name} is not a function tool")
        check(
            function.get("description") == description,
            f"unexpected {name} description: {function.get('description')!r}",
        )
        check(
            function.get("parameters") == schema,
            f"unexpected reflected {name} schema: {function.get('parameters')!r}",
        )


def extract_tool_envelope(request: ProviderRequest) -> dict[str, Any]:
    check(
        request.path == "/v1/chat/completions",
        f"unexpected follow-up endpoint: {request.path}",
    )
    messages = request.body.get("messages")
    check(isinstance(messages, list), f"messages is not an array: {messages!r}")
    tool_messages = [
        message
        for message in messages
        if isinstance(message, dict) and message.get("role") == "tool"
    ]
    check(
        len(tool_messages) == 1,
        f"expected one tool result in follow-up request: {tool_messages!r}",
    )
    message = tool_messages[0]
    check(
        message.get("tool_call_id") == TOOL_CALL_ID,
        f"tool result lost its call id: {message!r}",
    )
    content = message.get("content")
    check(
        isinstance(content, str), f"tool result content is not JSON text: {content!r}"
    )
    envelope = json.loads(content)
    check(isinstance(envelope, dict), f"tool result is not an object: {envelope!r}")

    check(
        set(envelope) == {"error", "result", "turn_tool_budget"},
        f"tool result is not the fixed typed envelope: {envelope!r}",
    )
    check(envelope["error"] is None, f"move unexpectedly failed: {envelope!r}")
    result = envelope["result"]
    check(isinstance(result, dict), f"move result is not an object: {result!r}")
    check(
        set(result) == {"item_here", "ok", "position", "reason"},
        f"move result has the wrong reflected shape: {result!r}",
    )
    check(result["ok"] is True, f"east move was not successful: {result!r}")
    check(
        result["position"] == {"x": 6, "y": 5},
        f"east move reached the wrong position: {result!r}",
    )
    check(result["reason"] is None, f"successful move has a reason: {result!r}")
    check(
        result["item_here"] is None
        or result["item_here"] in {"berry", "apple", "truffle", "toadstool"},
        f"move returned an invalid reflected item enum: {result!r}",
    )
    check(
        envelope["turn_tool_budget"]
        == {
            "used": 1,
            "remaining": 3,
            "instruction": "3 world-tool calls remain in this turn.",
        },
        f"unexpected typed turn budget: {envelope['turn_tool_budget']!r}",
    )
    return envelope


def assert_reflection_rejection(request: ProviderRequest) -> None:
    check(
        request.path == "/v1/chat/completions",
        f"unexpected rejection follow-up endpoint: {request.path}",
    )
    messages = request.body.get("messages")
    check(isinstance(messages, list), f"messages is not an array: {messages!r}")

    assistant_calls = [
        message
        for message in messages
        if isinstance(message, dict)
        and message.get("role") == "assistant"
        and message.get("tool_calls")
    ]
    check(
        len(assistant_calls) == 1,
        f"expected one rejected assistant tool call: {assistant_calls!r}",
    )
    tool_calls = assistant_calls[0]["tool_calls"]
    check(
        isinstance(tool_calls, list) and len(tool_calls) == 1,
        f"unexpected rejected tool-call structure: {tool_calls!r}",
    )
    call = tool_calls[0]
    check(call.get("id") == INVALID_TOOL_CALL_ID, f"wrong rejected call: {call!r}")
    function = call.get("function")
    check(isinstance(function, dict), f"rejected call has no function: {call!r}")
    check(function.get("name") == "move", f"wrong rejected function: {call!r}")
    arguments = function.get("arguments")
    check(isinstance(arguments, str), f"rejected arguments are not text: {call!r}")
    check(
        json.loads(arguments) == {"direction": "up"},
        f"unexpected rejected arguments: {arguments!r}",
    )

    tool_messages = [
        message
        for message in messages
        if isinstance(message, dict) and message.get("role") == "tool"
    ]
    check(
        len(tool_messages) == 1,
        f"expected one reflected error result: {tool_messages!r}",
    )
    result = tool_messages[0]
    check(
        result.get("tool_call_id") == INVALID_TOOL_CALL_ID,
        f"reflected error lost its call id: {result!r}",
    )
    content = result.get("content")
    check(isinstance(content, str), f"reflected error is not JSON text: {content!r}")
    check(
        json.loads(content) == {"error": "tool handler returned an error"},
        f"unexpected public reflected error result: {content!r}",
    )


def assert_valid_jsonl_log(
    records: list[dict[str, Any]], envelope: dict[str, Any], base_url: str
) -> None:
    tool_records = [record for record in records if record.get("type") == "tool"]
    check(len(tool_records) == 1, f"expected one tool log record: {records!r}")
    tool = tool_records[0]
    check(tool.get("turn") == 1, f"tool record has wrong turn: {tool!r}")
    check(tool.get("tick") == 1, f"tool record has wrong tick: {tool!r}")
    check(tool.get("tool") == "move", f"wrong tool was logged: {tool!r}")
    check(
        tool.get("args") == {"direction": "east"},
        f"reflected arguments were not logged: {tool!r}",
    )
    check(
        tool.get("result") == envelope,
        f"logged result differs from provider tool result: {tool!r}",
    )
    check(tool.get("before") == {"x": 5, "y": 5}, f"wrong before: {tool!r}")
    check(tool.get("after") == {"x": 6, "y": 5}, f"wrong after: {tool!r}")
    check(tool.get("action_executed") is True, f"move was not marked: {tool!r}")
    check(tool.get("score_after") == 0, f"move changed the score: {tool!r}")

    header = records[0]
    check(header.get("type") == "header", f"missing JSONL header: {records!r}")
    check(header.get("model") == EXPECTED_MODEL, f"wrong log model: {header!r}")
    check(header.get("base_url") == base_url, f"wrong log base URL: {header!r}")

    turn_records = [record for record in records if record.get("type") == "turn"]
    check(len(turn_records) == 1, f"expected one turn record: {records!r}")
    check(
        turn_records[0].get("assistant_text") == FINAL_TEXT,
        f"final provider text was not recorded: {turn_records[0]!r}",
    )
    check(turn_records[0].get("tool_calls") == 1, f"wrong turn count: {records!r}")

    footer = records[-1]
    check(footer.get("type") == "footer", f"missing JSONL footer: {records!r}")
    check(footer.get("complete") is True, f"incomplete JSONL footer: {footer!r}")
    check(
        footer.get("finish_reason") == "turn_budget",
        f"unexpected episode finish reason: {footer!r}",
    )
    check(
        footer.get("tool_call_counts") == {"move": 1, "look": 0, "eat": 0},
        f"tool counts do not reconcile: {footer!r}",
    )


def assert_rejected_jsonl_log(records: list[dict[str, Any]], base_url: str) -> None:
    header = records[0]
    check(header.get("type") == "header", f"missing rejection header: {records!r}")
    check(header.get("model") == EXPECTED_MODEL, f"wrong log model: {header!r}")
    check(header.get("base_url") == base_url, f"wrong log base URL: {header!r}")

    tool_records = [record for record in records if record.get("type") == "tool"]
    check(
        not tool_records,
        f"schema-rejected call reached Pig Pen's typed event layer: {tool_records!r}",
    )
    check(
        all(
            "before" not in record
            and "after" not in record
            and "action_executed" not in record
            for record in records
        ),
        f"schema-rejected call emitted a world transition: {records!r}",
    )

    turn_records = [record for record in records if record.get("type") == "turn"]
    check(len(turn_records) == 1, f"expected one rejected turn: {records!r}")
    turn = turn_records[0]
    check(turn.get("status") == "completed", f"rejected turn did not finish: {turn!r}")
    check(
        turn.get("assistant_text") == INVALID_FINAL_TEXT,
        f"rejection final text was not logged: {turn!r}",
    )
    check(turn.get("tool_calls") == 0, f"rejected call was counted: {turn!r}")
    check(turn.get("zero_tool_turn") is True, f"wrong zero-tool marker: {turn!r}")

    footer = records[-1]
    check(footer.get("type") == "footer", f"missing rejection footer: {records!r}")
    check(footer.get("complete") is True, f"incomplete rejection log: {footer!r}")
    check(
        footer.get("finish_reason") == "turn_budget",
        f"unexpected rejection finish reason: {footer!r}",
    )
    check(footer.get("final_score") == 0, f"rejected call changed score: {footer!r}")
    check(
        footer.get("tool_call_counts") == {"move": 0, "look": 0, "eat": 0},
        f"rejected call changed tool counts: {footer!r}",
    )
    check(
        footer.get("items_eaten")
        == {"berry": 0, "apple": 0, "truffle": 0, "toadstool": 0},
        f"rejected call changed the world outcome: {footer!r}",
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise RuntimeError("usage: reflection_integration_test.py PIGPEN_HEADLESS")

    executable = pathlib.Path(sys.argv[1]).resolve()
    check(executable.is_file(), f"headless executable does not exist: {executable}")

    valid = run_scenario(
        executable,
        [
            ProviderResponse(TOOL_STREAM, "req-reflection-tool"),
            ProviderResponse(FINAL_STREAM, "req-reflection-final"),
        ],
        "valid",
    )
    check(
        valid.completed.returncode == 0,
        f"valid integration run exited {valid.completed.returncode}\n"
        f"stdout={valid.completed.stdout}\nstderr={valid.completed.stderr}",
    )
    check(
        len(valid.requests) == 2,
        f"expected two valid provider requests, got {len(valid.requests)}",
    )
    assert_reflected_tools(valid.requests[0])
    envelope = extract_tool_envelope(valid.requests[1])
    assert_valid_jsonl_log(valid.records, envelope, valid.base_url)
    check(
        FINAL_TEXT in valid.completed.stdout,
        f"valid final text was not printed: {valid.completed.stdout}",
    )
    check(
        "summary finish_reason=turn_budget" in valid.completed.stdout,
        f"valid headless summary was not successful: {valid.completed.stdout}",
    )

    rejected = run_scenario(
        executable,
        [
            ProviderResponse(INVALID_TOOL_STREAM, "req-invalid-reflection-tool"),
            ProviderResponse(INVALID_FINAL_STREAM, "req-invalid-reflection-final"),
        ],
        "rejected",
    )
    check(
        rejected.completed.returncode == 5,
        f"rejection integration run exited {rejected.completed.returncode}\n"
        f"stdout={rejected.completed.stdout}\nstderr={rejected.completed.stderr}",
    )
    check(
        len(rejected.requests) == 2,
        f"expected two rejection provider requests, got {len(rejected.requests)}",
    )
    assert_reflected_tools(rejected.requests[0])
    assert_reflection_rejection(rejected.requests[1])
    assert_rejected_jsonl_log(rejected.records, rejected.base_url)
    check(
        INVALID_FINAL_TEXT in rejected.completed.stdout,
        f"rejection final text was not printed: {rejected.completed.stdout}",
    )
    check(
        "summary finish_reason=turn_budget turns_used=1 turn_budget=1 score=0 "
        "tool_calls=0" in rejected.completed.stdout,
        f"rejected call changed the headless world summary: {rejected.completed.stdout}",
    )
    check(
        "tool[turn=" not in rejected.completed.stdout,
        f"rejected call emitted a world transition: {rejected.completed.stdout}",
    )
    check(
        "validation error:" in rejected.completed.stderr,
        f"exit-5 diagnostic was not emitted: {rejected.completed.stderr}",
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
