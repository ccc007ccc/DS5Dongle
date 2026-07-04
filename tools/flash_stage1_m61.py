#!/usr/bin/env python3
"""Flash the ESP32 stage-1 firmware through the M61 CDC/UART bridge."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import time

import serial


ROOT = Path(__file__).resolve().parents[1]
BRIDGE_MARKERS = (
    b"M61 bridge status",
    b"M61 ESP32 bridge commands",
    b"OK ",
    b"ERR ",
)


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


def looks_like_bridge_response(output: bytes) -> bool:
    return any(marker in output for marker in BRIDGE_MARKERS)


def send_m61_command(port: str, baud: int, command: str, read_ms: int = 1000) -> bytes:
    with serial.Serial(port, baudrate=baud, timeout=0.05, rtscts=False, dsrdtr=False) as ser:
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.05)
        ser.reset_input_buffer()
        ser.write(f"~m61 {command}\n".encode("ascii"))
        ser.flush()
        deadline = time.monotonic() + read_ms / 1000.0
        chunks: list[bytes] = []
        while time.monotonic() < deadline:
            data = ser.read(4096)
            if data:
                chunks.append(data)
            else:
                time.sleep(0.02)
        return b"".join(chunks)


def require_bridge_ok(output: bytes, expected: bytes) -> bool:
    if output:
        sys.stdout.buffer.write(output)
        sys.stdout.buffer.flush()
    if expected in output and looks_like_bridge_response(output):
        return True

    print(
        "M61 bridge command did not return the expected response. "
        "Flash m61/esp32_prog_bridge first, then retry.",
        file=sys.stderr,
    )
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="M61 CDC serial port, for example COM7")
    parser.add_argument("-b", "--baud", default="115200", help="esptool baud rate")
    parser.add_argument("--control-baud", type=int, default=115200, help="baud used for M61 text commands")
    parser.add_argument("--python", default=sys.executable, help="Python executable with esptool installed")
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
    parser.add_argument("--no-run-after", action="store_true", help="leave ESP32 in ROM download mode")
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
    print(f"Putting ESP32 into ROM download mode via M61 on {args.port}...")
    try:
        output = send_m61_command(args.port, args.control_baud, "boot")
    except serial.SerialException as exc:
        print(f"M61 boot command failed: {exc}", file=sys.stderr)
        return 1
    if not require_bridge_ok(output, b"OK boot"):
        return 2

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
        "no_reset",
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
        return result.returncode

    if not args.no_run_after:
        print("Resetting ESP32 into app via M61...")
        try:
            output = send_m61_command(args.port, args.control_baud, "run")
        except serial.SerialException as exc:
            print(f"M61 run command failed: {exc}", file=sys.stderr)
            return 1
        if not require_bridge_ok(output, b"OK run"):
            return 2

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
