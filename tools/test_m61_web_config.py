#!/usr/bin/env python3
"""Build and run the pure M61 Web management protocol host tests through WSL."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive, tail = os.path.splitdrive(str(resolved))
    return f"/mnt/{drive[0].lower()}{tail.replace(chr(92), '/')}"


def main() -> int:
    project = ROOT / "m61" / "dualsense_hidp_probe"
    test = ROOT / "tools" / "test_m61_web_config.c"
    source = project / "m61_web_config.c"
    output = "/tmp/m61-web-config-test"
    command = (
        "set -eu; "
        f"trap 'rm -f {output}' EXIT; "
        f"cc -std=c11 -Wall -Wextra -Werror -I{wsl_path(project)} "
        f"{wsl_path(test)} {wsl_path(source)} -o {output}; "
        f"{output}"
    )
    return subprocess.run(["wsl", "sh", "-lc", command], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
