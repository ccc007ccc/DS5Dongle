#!/usr/bin/env python3
"""Run DS5Dongle project checks that do not require connected hardware."""

from __future__ import annotations

import argparse
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


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


def wsl_path(path: Path) -> str:
    result = subprocess.run(
        ["wsl", "--exec", "wslpath", "-a", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-pycompile", action="store_true", help="skip Python syntax checks")
    parser.add_argument(
        "--include-esp32-build",
        action="store_true",
        help="also build ESP32 default, raw HIDP, dual-chip, and dual-chip left-side pin firmware",
    )
    parser.add_argument(
        "--include-m61-build",
        action="store_true",
        help="also build M61 bridge, default HIDP probe, and dual-chip left-side profile through WSL",
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
    wsl_root = shlex.quote(wsl_path(ROOT))

    steps: list[tuple[str, list[str]]] = [
        ("project structure", [sys.executable, "tools/verify_project.py"]),
        ("DualSense parser vectors", [sys.executable, "tools/test_dualsense_protocol.py"]),
        ("M61 inactivity policy", [sys.executable, "tools/test_m61_inactivity.py"]),
        ("M61 PS shortcut policy", [sys.executable, "tools/test_m61_ps_shortcut.py"]),
        ("M61 wake descriptors", [sys.executable, "tools/test_m61_wake_descriptors.py"]),
        ("M61 flash flow", [sys.executable, "tools/test_m61_flash_flow.py"]),
        ("M61 transport generation", [sys.executable, "tools/test_m61_transport_generation.py"]),
        ("dual-chip scheduler model", [sys.executable, "tools/test_dual_chip_scheduler.py"]),
        ("M61 SPI scheduler contracts", [sys.executable, "tools/test_m61_spi_scheduler.py"]),
        ("scheduler RAM forecast", [sys.executable, "tools/scheduler_ram_budget.py"]),
        ("scheduler memory gate tests", [sys.executable, "tools/test_scheduler_memory.py"]),
        (
            "M61 audio epoch host test",
            [
                "wsl",
                "bash",
                "-lc",
                f"cd {wsl_root} && "
                "gcc -std=c11 -Wall -Wextra -Werror "
                "-DM61_AUDIO_EPOCH_HOST_TEST -I main -I m61/dualsense_hidp_probe "
                "tools/test_m61_audio_epoch.c "
                "m61/dualsense_hidp_probe/m61_audio_epoch.c "
                "main/dual_chip_scheduler_types.c "
                "-o /tmp/test_m61_audio_epoch && /tmp/test_m61_audio_epoch",
            ],
        ),
        ("stage-1 log checker self-test", [sys.executable, "tools/test_stage1_log_checker.py"]),
        ("M61 HIDP log checker self-test", [sys.executable, "tools/test_m61_hidp_log_checker.py"]),
        ("dual-chip log checker self-test", [sys.executable, "tools/check_dual_chip_log.py", "--self-test"]),
        ("dual-chip hardware validator self-test", [sys.executable, "tools/validate_dual_chip_hardware.py", "--self-test"]),
        ("M61 USB Windows checker self-test", [sys.executable, "tools/check_m61_usb_windows.py", "--self-test"]),
        ("M61 USB hardware validator self-test", [sys.executable, "tools/validate_m61_usb_hardware.py", "--self-test"]),
        ("DS5 Windows desktop tester smoke", [sys.executable, "tools/ds5_windows_test_app.py", "--smoke-test"]),
    ]

    if not args.skip_pycompile:
        steps.append(("Python syntax", py_compile_command()))

    if args.include_esp32_build:
        steps.extend([
            ("ESP32 HIDH build", [sys.executable, "tools/build_esp32_stage1.py"]),
            (
                "ESP32 raw HIDP build",
                [sys.executable, "tools/build_esp32_stage1.py", "--backend", "raw-hidp"],
            ),
            (
                "ESP32 dual-chip build",
                [sys.executable, "tools/build_esp32_stage1.py", "--backend", "dual-chip"],
            ),
            (
                "ESP32 dual-chip left-side pin build",
                [
                    sys.executable,
                    "tools/build_esp32_stage1.py",
                    "--backend",
                    "dual-chip",
                    "--pin-profile",
                    "devkit-left",
                ],
            ),
        ])

    if args.include_m61_build:
        steps.extend([
            (
                "M61 ESP32 bridge build",
                ["wsl", "bash", "/mnt/c/code/MCU/DS5Dongle/m61/esp32_prog_bridge/build.sh"],
            ),
            (
                "M61 DualSense HIDP probe build",
                ["wsl", "bash", "/mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh"],
            ),
            (
                "M61 DualSense dual-chip left-side build",
                [
                    "wsl",
                    "bash",
                    "/mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh",
                    "--profile",
                    "dual-chip-left-spi",
                ],
            ),
        ])

    if args.check_artifacts:
        steps.append(("firmware artifact manifest", [sys.executable, "tools/firmware_manifest.py", "--strict"]))
        steps.append(
            (
                "dual-chip release artifact gate",
                [sys.executable, "tools/check_dual_chip_release.py"],
            )
        )

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
