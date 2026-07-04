#!/usr/bin/env python3
"""Capture and check the ESP32 stage-1 hardware validation log."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import capture_stage1_log
import check_stage1_log


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="ESP32 serial port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="ESP32 log baud")
    parser.add_argument("-o", "--output", type=Path, default=Path("stage1_validation.log"))
    parser.add_argument("--duration", type=int, default=300, help="capture duration in seconds")
    parser.add_argument("--reset", action="store_true", help="pulse EN/RST through RTS before capture")
    parser.add_argument("--min-reports", type=int, default=20, help="minimum parsed input reports")
    parser.add_argument(
        "--allow-close",
        action="store_true",
        help="allow HID close markers in deliberately disconnected logs",
    )
    parser.add_argument(
        "--basic-only",
        action="store_true",
        help="bring-up diagnostic mode: do not require output init or full 0x31 reports",
    )
    parser.add_argument(
        "--no-require-input-activity",
        action="store_true",
        help="do not require stick, trigger, button, and motion activity in the captured log",
    )
    args = parser.parse_args(argv)

    capture_args = [
        "-p",
        args.port,
        "-b",
        str(args.baud),
        "-o",
        str(args.output),
        "--duration",
        str(args.duration),
    ]
    if args.reset:
        capture_args.append("--reset")

    print("Stage 1 hardware validation capture starting.")
    print("Operate the DualSense during the capture: sticks, buttons, triggers, and motion.")
    capture_result = capture_stage1_log.main(capture_args)
    if capture_result != 0:
        return capture_result

    check_args = [
        str(args.output),
        "--min-reports",
        str(args.min_reports),
        "--min-duration-ms",
        str(args.duration * 1000),
    ]
    if args.allow_close:
        check_args.append("--allow-close")
    if not args.no_require_input_activity:
        check_args.append("--require-input-activity")
    if not args.basic_only:
        check_args.extend(["--require-output-init", "--require-full-report"])

    check_result = check_stage1_log.main(check_args)
    if check_result != 0:
        return check_result

    if args.basic_only:
        print("Basic stage-1 capture passed. This is not the final acceptance gate.")
    else:
        print("Stage 1 hardware log gate passed.")
        print("Manual confirmation still required: input mapping, visible LEDs, latency, and packet loss.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
