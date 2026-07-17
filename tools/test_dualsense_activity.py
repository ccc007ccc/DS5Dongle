#!/usr/bin/env python3
"""Build and run the M61 idle-activity boundary tests through WSL."""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive, tail = os.path.splitdrive(str(resolved))
    return f"/mnt/{drive[0].lower()}{tail.replace(chr(92), '/')}"


def main() -> None:
    app = ROOT / "m61" / "dualsense_hidp_probe"
    command = [
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", wsl_path(app),
        wsl_path(ROOT / "tools" / "test_dualsense_activity.c"),
        wsl_path(app / "dualsense_parser.c"),
        "-o", "/tmp/m61-dualsense-activity-test",
    ]
    subprocess.run(["wsl", *command], check=True)
    subprocess.run(["wsl", "/tmp/m61-dualsense-activity-test"], check=True)
    print("M61 DualSense idle-activity tests passed")


if __name__ == "__main__":
    main()
