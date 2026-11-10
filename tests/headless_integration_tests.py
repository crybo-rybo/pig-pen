"""Drive pig-pen-headless end to end against the scripted fake llamad.

Covers what the C++ suite cannot: the real CLI process over gRPC on a Unix
socket, argv -> Config wiring, stdout, exit codes 0 and 5, and a JSONL log
written to disk.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

from fake_llamad import MODEL_INFO, FakeLlamad, check, text_reply, tool_call_reply

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
# The model-visible contract, in declaration order: (name, description,
# parameters) as each llamad request must carry them.
EXPECTED_TOOLS = [
    ("move", "Move one cell north, south, east, or west.", DIRECTION_SCHEMA),
    ("look", "Scan every cell in one direction to the wall.", DIRECTION_SCHEMA),
    ("eat", "Eat the item on the current cell, if present.", EAT_SCHEMA),
]


def tool_stats(invalid_calls: int) -> dict[str, Any]:
    """The tool loop's accounting for a turn with one requested call."""
    return {
        "rounds": 1,
        "calls": 1,
        "rejected_calls": 0,
        "invalid_calls": invalid_calls,
        "round_limit_reached": False,
        "unexecuted_calls": 0,
    }


def run(
    executables: tuple[str, str], replies: list[dict], *extra: str
) -> tuple[subprocess.CompletedProcess[str], list[dict], list[dict], str]:
    """Run one single-turn episode; return (process, requests, records, socket)."""
    headless, fake = executables
    with (
        FakeLlamad(fake, replies) as daemon,
        tempfile.TemporaryDirectory(prefix="pigpen-headless-") as log_dir,
    ):
        completed = subprocess.run(
            [
                headless,
                *("--socket", daemon.socket, "--seed", "0"),
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
        return completed, daemon.requests(), records, daemon.socket


def subset(record: dict[str, Any], expected: dict[str, Any]) -> dict[str, Any]:
    return {key: record.get(key) for key in expected}


def check_request(body: dict[str, Any]) -> None:
    sent = [
        (tool.get("name"), tool.get("description"), tool.get("parameters"))
        for tool in body.get("tools", [])
    ]
    # A list comparison also pins cardinality and rejects duplicate tools.
    check(sent == EXPECTED_TOOLS, f"unexpected model-visible tools: {sent!r}")
    roles = [message["role"] for message in body["messages"]]
    check(roles[:2] == ["system", "user"], f"unexpected roles: {roles!r}")


def only_tool_message(body: dict[str, Any]) -> dict[str, Any]:
    messages = [m for m in body["messages"] if m.get("role") == "tool"]
    check(len(messages) == 1, f"expected one tool message: {messages!r}")
    return messages[0]


def test_valid_move(executables: tuple[str, str]) -> None:
    completed, requests, records, socket = run(
        executables,
        [
            tool_call_reply(VALID_CALL_ID, "move", {"direction": "east"}),
            text_reply(FINAL_TEXT),
        ],
        "--sampling-seed",
        str(SAMPLING_SEED),
    )
    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 0, f"exit {completed.returncode}\n{output}")
    check(len(requests) == 2, f"expected two requests, got {len(requests)}")
    for body in requests:
        check_request(body)
    check(
        all(body["sampling"].get("seed") == SAMPLING_SEED for body in requests),
        "sampling seed was not sent on every request",
    )
    check(
        all(body["sampling"].get("max_tokens") == 8192 for body in requests),
        "max_tokens was not sent on every request",
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
    assistant = [m for m in requests[1]["messages"] if m["role"] == "assistant"]
    check(
        [c["id"] for m in assistant for c in m["tool_calls"]] == [VALID_CALL_ID],
        f"assistant history: {assistant!r}",
    )

    canonical = json.dumps(result, separators=(",", ":"), sort_keys=True)
    activity = (
        'tool[turn=1,tick=1] move args={"direction":"east"} '
        f"result={canonical} position=(5,5)->(6,5)"
    )
    for line in (
        f'session model="{MODEL_INFO["description"]}" socket="{socket}" n_ctx=32768',
        activity,
        FINAL_TEXT,
        "summary finish_reason=turn_budget",
    ):
        check(line in completed.stdout, f"missing {line!r} in stdout\n{output}")

    header, *body, footer = records
    expected_header = {
        "type": "header",
        "format": 2,
        "model": MODEL_INFO["description"],
        "socket": socket,
        "sampling_seed": SAMPLING_SEED,
        "model_info": {
            key: MODEL_INFO[key]
            for key in ("n_ctx", "n_ctx_train", "n_params", "size_bytes")
        },
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
        "call_id": VALID_CALL_ID,
        "round": 1,
        "index": 0,
        "args": {"direction": "east"},
        "result": result,
        "before": {"x": 5, "y": 5},
        "after": {"x": 6, "y": 5},
        "score_after": 0,
    }
    check(tools == [expected_tool], f"tool records: {tools!r}")
    expected_turn = {
        "assistant_text": FINAL_TEXT,
        "requests": 2,
        "tool_calls": 1,
        "input_tokens": 60,
        "output_tokens": 7,
        "cached_input_tokens": 18,
        "tool_stats": tool_stats(invalid_calls=0),
    }
    check([subset(t, expected_turn) for t in turns] == [expected_turn], f"{turns!r}")
    expected_footer = {
        "type": "footer",
        "complete": True,
        "finish_reason": "turn_budget",
        "tool_call_counts": {"eat": 0, "look": 0, "move": 1},
    }
    check(subset(footer, expected_footer) == expected_footer, f"footer: {footer!r}")


def test_schema_rejection_exits_5(executables: tuple[str, str]) -> None:
    completed, requests, records, _ = run(
        executables,
        [
            tool_call_reply(INVALID_CALL_ID, "move", {"direction": "up"}),
            text_reply(INVALID_FINAL_TEXT),
        ],
    )
    output = f"stdout={completed.stdout}\nstderr={completed.stderr}"
    check(completed.returncode == 5, f"exit {completed.returncode}\n{output}")
    check(len(requests) == 2, f"expected two requests, got {len(requests)}")
    check_request(requests[0])
    check(all("seed" not in body["sampling"] for body in requests), "unset seed")

    history = requests[1]["messages"]
    assistant_calls = [
        call
        for m in history
        if m.get("role") == "assistant"
        for call in m.get("tool_calls") or []
    ]
    check(
        [(c["id"], c["name"], json.loads(c["arguments"])) for c in assistant_calls]
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
        "requests": 2,
        "tool_stats": tool_stats(invalid_calls=1),
    }
    check([subset(t, expected_turn) for t in turns] == [expected_turn], f"{turns!r}")
    expected_footer = {
        "type": "footer",
        "complete": True,
        "finish_reason": "turn_budget",
        "final_score": 0,
        "tool_call_counts": {"eat": 0, "look": 0, "move": 0},
    }
    check(subset(footer, expected_footer) == expected_footer, f"footer: {footer!r}")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: headless_integration_tests.py PIG_PEN_HEADLESS PIGPEN_FAKE_LLAMAD"
        )
    executables = (
        str(pathlib.Path(sys.argv[1]).resolve()),
        str(pathlib.Path(sys.argv[2]).resolve()),
    )
    test_valid_move(executables)
    test_schema_rejection_exits_5(executables)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
