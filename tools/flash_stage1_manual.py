#!/usr/bin/env python3
"""Flash the stage-1 ESP32 firmware after manual BOOT/IO0 entry.

Use this when the ESP32 development board's auto-reset wiring does not enter
ROM download mode reliably. The script waits for a countdown so the operator
can hold BOOT/IO0, tap EN/RST, keep BOOT/IO0 held, and then let esptool sync.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]


def require_file(path: Path) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"missing required image: {path}")


def default_build_dir(backend: str) -> Path:
    if backend == "raw-hidp":
        return ROOT / "build_raw_hidp"
    return ROOT / "build"


def flash_images(build_dir: Path) -> list[tuple[str, Path]]:
    return [
        ("0x1000", build_dir / "bootloader" / "bootloader.bin"),
        ("0x8000", build_dir / "partition_table" / "partition-table.bin"),
        ("0x10000", build_dir / "ds5_dualsense_bridge_esp32.bin"),
    ]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", default="COM5", help="serial port, for example COM5")
    parser.add_argument("-b", "--baud", default="115200", help="esptool baud rate")
    parser.add_argument(
        "--backend",
        choices=("hidh", "raw-hidp"),
        default="hidh",
        help="stage-1 backend image set to flash",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="override ESP-IDF build directory containing bootloader, partition table, and app",
    )
    parser.add_argument(
        "--countdown",
        type=int,
        default=5,
        help="seconds to wait before esptool starts",
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python executable that has esptool installed",
    )
    args = parser.parse_args(argv)

    build_dir = args.build_dir if args.build_dir is not None else default_build_dir(args.backend)
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    images = flash_images(build_dir)
    try:
        for _, path in images:
            require_file(path)
    except FileNotFoundError as exc:
        print(f"Flash failed: {exc}")
        if args.backend == "raw-hidp" and args.build_dir is None:
            print("Run python tools\\build_esp32_stage1.py --backend raw-hidp first.")
        else:
            print("Run python tools\\build_esp32_stage1.py first.")
        return 1

    print(f"Flashing ESP32 stage-1 backend={args.backend} build_dir={build_dir}")
    print("Manual ESP32 download-mode sequence:")
    print("  1. Hold BOOT/IO0.")
    print("  2. Tap EN/RST and release EN/RST.")
    print("  3. Keep holding BOOT/IO0 until esptool prints 'Writing at'.")
    for remaining in range(args.countdown, 0, -1):
        print(f"Starting flash in {remaining:2d}s", end="\r", flush=True)
        time.sleep(1)
    print("Starting flash now.                                      ")

    cmd = [
        args.python,
        "-m",
        "esptool",
        "--chip",
        "esp32",
        "-p",
        args.port,
        "-b",
        args.baud,
        "--before",
        "no_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "40m",
        "--flash_size",
        "2MB",
    ]
    for offset, path in images:
        cmd.extend([offset, str(path)])

    return subprocess.run(cmd, cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
