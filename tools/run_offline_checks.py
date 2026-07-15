#!/usr/bin/env python3
"""Run DS5Dongle project checks that do not require connected hardware."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive, tail = os.path.splitdrive(str(resolved))
    if not drive:
        return str(resolved).replace("\\", "/")
    return f"/mnt/{drive[0].lower()}{tail.replace(chr(92), '/')}"


def run_step(name: str, cmd: list[str]) -> int:
    print(f"\n== {name} ==")
    print("running:", " ".join(cmd), flush=True)
    result = subprocess.run(cmd, cwd=ROOT)
    if result.returncode != 0:
        print(f"{name} failed with exit code {result.returncode}", file=sys.stderr)
    return result.returncode


def py_compile_command() -> list[str]:
    tool_files = sorted((ROOT / "tools").glob("*.py"))
    return [sys.executable, "-m", "py_compile", *[str(path) for path in tool_files]]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-pycompile", action="store_true", help="skip Python syntax checks")
    parser.add_argument(
        "--include-m61-build",
        action="store_true",
        help="also build both M61 firmware probes through WSL",
    )
    parser.add_argument(
        "--check-artifacts",
        action="store_true",
        help="also require expected firmware artifacts and print their manifest",
    )
    parser.add_argument(
        "--continue-on-failure",
        action="store_true",
        help="run every selected step before returning failure",
    )
    args = parser.parse_args(argv)

    steps: list[tuple[str, list[str]]] = [
        ("project structure", [sys.executable, "tools/verify_project.py"]),
        ("requirements audit", [sys.executable, "tools/audit_requirements.py", "--require-spec"]),
        ("requirements audit self-test", [sys.executable, "tools/test_requirements_audit.py"]),
        ("DualSense parser vectors", [sys.executable, "tools/test_dualsense_protocol.py"]),
        ("M61 HIDP log checker self-test", [sys.executable, "tools/test_m61_hidp_log_checker.py"]),
        ("M61 USB Windows checker self-test", [sys.executable, "tools/check_m61_usb_windows.py", "--self-test"]),
        ("M61 USB hardware validator self-test", [sys.executable, "tools/validate_m61_usb_hardware.py", "--self-test"]),
        ("DS5 Windows desktop tester smoke", [sys.executable, "tools/ds5_windows_test_app.py", "--smoke-test"]),
        ("M61 realtime memory gate self-test", [sys.executable, "tools/test_m61_realtime_memory.py"]),
        ("M61 realtime scheduler host tests", [sys.executable, "tools/test_m61_realtime_scheduler.py"]),
    ]

    if not args.skip_pycompile:
        steps.append(("Python syntax", py_compile_command()))

    if args.include_m61_build:
        m61_hidp_build = wsl_path(ROOT / "m61" / "dualsense_hidp_probe" / "build.sh")
        steps.extend([
            (
                "M61 DualSense HIDP probe build",
                ["wsl", "bash", m61_hidp_build],
            ),
            (
                "M61 DualSense HIDP RAM gate",
                [
                    sys.executable,
                    "tools/check_m61_realtime_memory.py",
                    "m61/dualsense_hidp_probe/build/build_out/"
                    "m61_dualsense_hidp_probe_bl616.elf",
                ],
            ),
        ])

    if args.check_artifacts:
        steps.append(("firmware artifact manifest", [sys.executable, "tools/firmware_manifest.py", "--strict"]))

    first_failure = 0
    for name, cmd in steps:
        code = run_step(name, cmd)
        if code != 0:
            if first_failure == 0:
                first_failure = code
            if not args.continue_on_failure:
                return code

    if first_failure:
        return first_failure

    print("\nOffline checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
