#!/usr/bin/env python3
"""Capture raw ESP32 stage-1 serial logs to a file."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time

import serial


def hard_reset(ser: serial.Serial) -> None:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.2)
    ser.setRTS(False)
    time.sleep(0.2)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", default="COM5", help="serial port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="serial baud rate")
    parser.add_argument("-o", "--output", type=Path, default=Path("stage1.log"))
    parser.add_argument("--duration", type=int, default=120, help="capture duration in seconds")
    parser.add_argument("--reset", action="store_true", help="pulse EN/RST through RTS before capture")
    args = parser.parse_args(argv)

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.rtscts = False
    ser.dsrdtr = False
    ser.dtr = False
    ser.rts = False

    with ser:
        if args.reset:
            hard_reset(ser)
        ser.reset_input_buffer()

        start = time.monotonic()
        header = (
            f"Capturing {args.port} at {args.baud} for {args.duration}s -> {args.output}\n"
        ).encode("utf-8")
        sys.stdout.buffer.write(header)
        sys.stdout.buffer.flush()

        with args.output.open("wb") as log:
            while time.monotonic() - start < args.duration:
                data = ser.read(4096)
                if not data:
                    continue
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                log.write(data)
                log.flush()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
