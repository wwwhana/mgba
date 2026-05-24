#!/usr/bin/env python3
"""Run a FlashGBX source bundle from inside an mGBA package."""

import os
import shutil
import subprocess
import sys


MINIMUM_PYTHON = (3, 10)


def _candidate_pythons() -> list[str]:
    candidates = [
        os.environ.get("FLASHGBX_PYTHON", ""),
        "/opt/homebrew/bin/python3",
        "/opt/homebrew/bin/python3.14",
        "/opt/homebrew/bin/python3.13",
        "/opt/homebrew/bin/python3.12",
        "/opt/homebrew/bin/python3.11",
        "/opt/homebrew/bin/python3.10",
        "/usr/local/bin/python3",
        "/usr/local/bin/python3.14",
        "/usr/local/bin/python3.13",
        "/usr/local/bin/python3.12",
        "/usr/local/bin/python3.11",
        "/usr/local/bin/python3.10",
        shutil.which("python3") or "",
    ]
    seen = set()
    result = []
    for candidate in candidates:
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        if os.path.exists(candidate):
            result.append(candidate)
    return result


def _supports_flashgbx_syntax(python: str) -> bool:
    try:
        completed = subprocess.run(
            [python, "-c", "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return False
    return completed.returncode == 0


def _ensure_modern_python() -> None:
    if sys.version_info >= MINIMUM_PYTHON:
        return
    current = os.path.realpath(sys.executable)
    for python in _candidate_pythons():
        if os.path.realpath(python) == current:
            continue
        if _supports_flashgbx_syntax(python):
            os.execv(python, [python, os.path.abspath(__file__), *sys.argv[1:]])
    version = ".".join(str(part) for part in sys.version_info[:3])
    print(
        f"FlashGBX requires Python {MINIMUM_PYTHON[0]}.{MINIMUM_PYTHON[1]} or newer; "
        f"current interpreter is Python {version}.",
        file=sys.stderr,
    )
    raise SystemExit(1)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    for standalone in (
        os.path.join(here, "flashgbx-cli", "flashgbx-cli"),
        os.path.join(here, "flashgbx-cli", "flashgbx-cli.exe"),
    ):
        if not getattr(sys, "frozen", False) and os.path.exists(standalone):
            os.execv(standalone, [standalone, *sys.argv[1:]])

    _ensure_modern_python()
    for path in (os.path.join(here, "site-packages"), here):
        if path not in sys.path:
            sys.path.insert(0, path)

    from FlashGBX.FlashGBX import main as flashgbx_main

    flashgbx_main(portableMode=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
