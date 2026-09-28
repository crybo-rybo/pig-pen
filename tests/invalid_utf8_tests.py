"""Check that a CLI rejects command-line text that is not UTF-8 (exit 2).

Every such value ends up in a model request or a JSONL record, so it must be
refused as a usage error before any session, log, or record exists, rather
than surfacing later as a crash while serialising. Needs raw byte arguments,
so it only runs on POSIX.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

INVALID = b"pig\xff-model"
# Options per binary, and the arguments that make the rest of the line valid.
FLAVOURS = {
    "headless": (
        [b"--model", b"--base-url", b"--prompt-variant", b"--input"],
        [],
    ),
    "worker": (
        [b"--model", b"--base-url", b"--prompt-variant"],
        [b"--seeds", b"1"],
    ),
}


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[2] not in FLAVOURS:
        raise SystemExit("usage: invalid_utf8_tests.py EXECUTABLE headless|worker")
    executable = pathlib.Path(sys.argv[1]).resolve()
    options, base = FLAVOURS[sys.argv[2]]
    for option in options:
        with tempfile.TemporaryDirectory(prefix="pigpen-utf8-") as log_dir:
            # The bad value comes last, so it replaces the valid --model.
            completed = subprocess.run(
                [
                    bytes(executable),
                    *(b"--model", b"m", *base, b"--log-dir", log_dir.encode()),
                    *(option, INVALID),
                ],
                capture_output=True,
                timeout=20,
                check=False,
            )
            written = list(pathlib.Path(log_dir).iterdir())
        output = f"stdout={completed.stdout!r}\nstderr={completed.stderr!r}"
        check(completed.returncode == 2, f"{option!r}: exit {completed.returncode}")
        diagnostic = option + b" must be valid UTF-8"
        check(diagnostic in completed.stderr, f"{option!r}: {output}")
        check(completed.stdout == b"", f"{option!r} wrote to stdout: {output}")
        check(written == [], f"{option!r} wrote a log: {written!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
