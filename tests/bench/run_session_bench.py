"""Measure session creation cost and pump-pass latency against a loopback stub.

Not run by CTest; this reproduces the numbers in docs/training.md
("Performance notes"):

    cmake --preset release -B build/bench -DPIGPEN_BUILD_BENCH=ON
    cmake --build build/bench --target pigpen_session_bench
    python3 tests/bench/run_session_bench.py build/bench/pigpen_session_bench

The stub answers every request at once, so it measures the worker's own
overhead with no inference time at all: a request whose last message is a
tool result gets a text answer streamed in CHUNKS deltas, any other a
`move` + `eat` round. Each response is one write on a TCP_NODELAY socket;
headers and body sent in separate small writes would stall on Nagle's
algorithm and delayed ACKs (about 40 ms a request) and hide what is being
measured.
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


def frames(deltas: list[dict[str, Any]], finish: str) -> bytes:
    chunks = [{"choices": [{"index": 0, "delta": delta}]} for delta in deltas]
    chunks.append({"choices": [{"index": 0, "delta": {}, "finish_reason": finish}]})
    chunks.append(
        {"choices": [], "usage": {"prompt_tokens": 20, "completion_tokens": 4}}
    )
    body = "".join(
        "data: "
        + json.dumps({"id": "bench", "object": "chat.completion.chunk", **chunk})
        + "\n\n"
        for chunk in chunks
    )
    return (body + "data: [DONE]\n\n").encode()


def tool_round() -> bytes:
    calls = [
        ("a", "move", '{"direction":"east"}'),
        ("b", "eat", "{}"),
    ]
    delta = {
        "role": "assistant",
        "tool_calls": [
            {
                "index": index,
                "id": call_id,
                "type": "function",
                "function": {"name": name, "arguments": arguments},
            }
            for index, (call_id, name, arguments) in enumerate(calls)
        ],
    }
    return frames([delta], "tool_calls")


def text_answer(chunks: int) -> bytes:
    deltas = [{"role": "assistant", "content": "ok"}]
    deltas += [{"content": " tok"}] * (chunks - 1)
    return frames(deltas, "stop")


class StubServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 1024

    def __init__(self, chunks: int) -> None:
        super().__init__(("127.0.0.1", 0), StubHandler)
        self.replies = {"tool": tool_round(), "text": text_answer(chunks)}


class StubHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: StubServer

    def setup(self) -> None:
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def do_POST(self) -> None:
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        last = body["messages"][-1].get("role")
        reply = self.server.replies["text" if last == "tool" else "tool"]
        head = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            f"Content-Length: {len(reply)}\r\n\r\n"
        ).encode()
        self.wfile.write(head + reply)

    def log_message(self, format: str, *args: object) -> None:
        pass


def run(binary: str, *arguments: str) -> dict[str, Any]:
    completed = subprocess.run(
        [binary, *arguments], capture_output=True, text=True, check=True
    )
    return json.loads(completed.stdout)


def quantiles(samples: dict[str, Any], unit: float = 1000.0) -> str:
    return "  ".join(
        f"{name}={samples[f'{name}_us'] / unit:.2f}" for name in ("p50", "p99", "max")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", help="path to pigpen_session_bench")
    parser.add_argument("--parallel", type=int, nargs="+", default=[8, 32, 128])
    parser.add_argument("--chunks", type=int, nargs="+", default=[1, 200])
    parser.add_argument("--turns", type=int, default=5)
    parser.add_argument("--episodes-per-slot", type=int, default=4)
    parser.add_argument("--create-count", type=int, default=300)
    args = parser.parse_args()

    for chunks in args.chunks:
        server = StubServer(chunks)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}/v1"
        try:
            if chunks == args.chunks[0]:
                result = run(args.binary, "create", url, str(args.create_count))
                print("session lifecycle, ms:")
                for key in (
                    "idle_create",
                    "idle_destroy",
                    "create",
                    "one_turn_episode",
                    "destroy_after_episode",
                ):
                    print(f"  {key:<22} {quantiles(result[key])}")
            print(f"batch, text answers in {chunks} deltas, ms:")
            for parallel in args.parallel:
                episodes = parallel * args.episodes_per_slot
                result = run(
                    args.binary,
                    *("batch", url, str(parallel), str(episodes), str(args.turns)),
                )
                print(
                    f"  P={parallel:<4} {result['episodes_per_s']:7.1f} episodes/s"
                    f"  pump busy {result['pump_busy_percent']:5.1f}%"
                    f"  pass {quantiles(result['pass'])}"
                    f"  pump with work {quantiles(result['pump_with_callbacks'])}"
                )
        finally:
            server.shutdown()
            server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
