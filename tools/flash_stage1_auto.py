#!/usr/bin/env python3
"""Flash ESP32 stage-1 firmware using the board auto-reset circuit.

This path relies on the USB-UART bridge wiring DTR/RTS to ESP32 EN and IO0.
Many ESP32-WROOM-32 CH340 boards support it; some boards do not. If sync fails,
use tools/flash_stage1_manual.py or the M61 bridge path instead.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


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


def print_missing_build_hint(backend: str, build_dir_overridden: bool) -> None:
    if backend == "raw-hidp" and not build_dir_overridden:
        print("Run python tools\\build_esp32_stage1.py --backend raw-hidp first.")
    else:
        print("Run python tools\\build_esp32_stage1.py first.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="ESP32 USB-UART serial port, for example COM5")
    parser.add_argument("-b", "--baud", default="460800", help="esptool baud rate")
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
        "--python",
        default=sys.executable,
        help="Python executable that has esptool installed",
    )
    parser.add_argument(
        "--before",
        default="default_reset",
        choices=("default_reset", "no_reset"),
        help="esptool reset strategy before flashing",
    )
    parser.add_argument(
        "--after",
        default="hard_reset",
        choices=("hard_reset", "no_reset"),
        help="esptool reset strategy after flashing",
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
        print_missing_build_hint(args.backend, args.build_dir is not None)
        return 1

    print(f"Auto flashing ESP32 stage-1 backend={args.backend} build_dir={build_dir}")
    print("Using esptool --before default_reset by default; this requires DTR/RTS auto-reset wiring.")

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
        args.before,
        "--after",
        args.after,
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

    result = subprocess.run(cmd, cwd=ROOT)
    if result.returncode != 0:
        print("")
        print("Automatic ESP32 flash failed.")
        print("If esptool could not sync, this board's USB-UART path may not drive EN/IO0 correctly.")
        print("Fallbacks:")
        print(f"  python tools\\flash_stage1_manual.py -p {args.port} -b 115200 --backend {args.backend}")
        print(f"  python tools\\flash_stage1_m61.py -p {args.port} -b 115200 --backend {args.backend}")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
