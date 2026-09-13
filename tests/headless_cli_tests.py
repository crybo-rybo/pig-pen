"""Assert headless CLI parse failures have the intended cause and exit code."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: headless_cli_tests.py HEADLESS_BINARY")

    binary = Path(sys.argv[1])
    cases = (
        ((), "option error: --model is required"),
        (
            ("--model", "test-model", "--max-tool-rounds", "65"),
            "option error: --max-tool-rounds must be in the range 1..64",
        ),
        (
            ("--model", "test-model", "--temperature", "nan"),
            "option error: --temperature must be a finite number in the range 0.0..2.0",
        ),
    )

    for arguments, expected_diagnostic in cases:
        completed = subprocess.run(
            (str(binary), *arguments),
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        first_stderr_line = completed.stderr.partition("\n")[0]
        if completed.returncode != 2 or first_stderr_line != expected_diagnostic:
            raise AssertionError(
                f"arguments {arguments!r}: expected exit 2 and first stderr line "
                f"{expected_diagnostic!r}, got exit {completed.returncode}, "
                f"stdout {completed.stdout!r}, stderr {completed.stderr!r}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
