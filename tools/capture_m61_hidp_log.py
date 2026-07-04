#!/usr/bin/env python3
"""Capture Ai-M61 DualSense HIDP probe serial logs to a file."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time

import serial


def read_and_write(ser: serial.Serial, log, end_time: float, echo_stdout: bool) -> None:
    while time.monotonic() < end_time:
        data = ser.read(4096)
        if not data:
            continue
        if echo_stdout:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        log.write(data)
        log.flush()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="M61 serial port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="M61 probe console baud")
    parser.add_argument("-o", "--output", type=Path, default=Path("m61_hidp.log"))
    parser.add_argument("--duration", type=int, default=120, help="capture duration in seconds")
    parser.add_argument(
        "--kick-auto",
        action="store_true",
        help="send 'ds5 auto now' after opening the port",
    )
    parser.add_argument(
        "--command",
        action="append",
        default=[],
        help="extra ASCII command to send before capture; may be passed more than once",
    )
    parser.add_argument(
        "--usb-status",
        action="store_true",
        help="send 'ds5 log quiet' and 'ds5 status' before capture",
    )
    parser.add_argument(
        "--no-stdout",
        action="store_true",
        help="write the capture file without echoing serial data to stdout",
    )
    args = parser.parse_args(argv)

    with serial.Serial(args.port, baudrate=args.baud, timeout=0.2, rtscts=False, dsrdtr=False) as ser:
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.05)
        ser.reset_input_buffer()

        commands = list(args.command)
        if args.kick_auto:
            commands.insert(0, "ds5 auto now")
        if args.usb_status:
            commands.extend(["ds5 log quiet", "ds5 status"])
        for command in commands:
            ser.write(command.encode("ascii") + b"\r\n")
            ser.flush()
            time.sleep(0.1)

        print(f"Capturing {args.port} at {args.baud} for {args.duration}s -> {args.output}")
        with args.output.open("wb") as log:
            read_and_write(ser, log, time.monotonic() + args.duration, not args.no_stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
