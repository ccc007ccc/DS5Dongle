#!/usr/bin/env python3
"""Send ESP32 reset/download commands to the M61 bridge."""

from __future__ import annotations

import argparse
import sys
import time

import serial


BRIDGE_MARKERS = (
    b"M61 bridge status",
    b"M61 ESP32 bridge commands",
    b"OK ",
    b"ERR ",
)


def send_command(port: str, baud: int, command: str, read_ms: int) -> bytes:
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


def looks_like_bridge_response(output: bytes) -> bool:
    return any(marker in output for marker in BRIDGE_MARKERS)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="M61 host serial port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="M61 host serial baud")
    parser.add_argument("--read-ms", type=int, default=1000, help="milliseconds to read command output")
    parser.add_argument(
        "--allow-empty",
        action="store_true",
        help="return success even if the bridge does not answer",
    )
    parser.add_argument(
        "command",
        choices=["boot", "boot-hold", "reset", "run", "status", "help"],
        help="bridge command to send",
    )
    args = parser.parse_args(argv)

    try:
        output = send_command(args.port, args.baud, args.command, args.read_ms)
    except serial.SerialException as exc:
        print(f"M61 command failed: {exc}", file=sys.stderr)
        return 1

    if output:
        sys.stdout.buffer.write(output)
        sys.stdout.buffer.flush()
    if not args.allow_empty:
        if not output:
            print(
                "M61 bridge did not answer. The board is probably not running "
                "m61/esp32_prog_bridge, or COM/baud is wrong.",
                file=sys.stderr,
            )
            return 2
        if not looks_like_bridge_response(output):
            print(
                "M61 serial output did not contain a bridge response marker; "
                "not treating this as a working ESP32 programming bridge.",
                file=sys.stderr,
            )
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
