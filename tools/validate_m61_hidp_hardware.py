#!/usr/bin/env python3
"""Capture and check an Ai-M61 DualSense HIDP probe hardware log."""

from __future__ import annotations

import argparse
from pathlib import Path

import capture_m61_hidp_log
import check_m61_hidp_log


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="M61 serial port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="M61 probe console baud")
    parser.add_argument("-o", "--output", type=Path, default=Path("m61_hidp_validation.log"))
    parser.add_argument("--duration", type=int, default=300, help="capture duration in seconds")
    parser.add_argument("--min-reports", type=int, default=20, help="minimum parsed input reports")
    parser.add_argument("--no-kick-auto", action="store_true", help="do not send 'ds5 auto now'")
    parser.add_argument("--no-require-security", action="store_true", help="do not require security callback")
    parser.add_argument("--no-require-input-activity", action="store_true")
    parser.add_argument(
        "--allow-connected-stream",
        action="store_true",
        help="accept captures that start after M61 is already connected to the DualSense",
    )
    parser.add_argument(
        "--basic-only",
        action="store_true",
        help="diagnostic mode: do not require HIDP channels or full 0x31 reports",
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
    if not args.no_kick_auto:
        capture_args.append("--kick-auto")

    print("M61 HIDP hardware validation capture starting.")
    print("Put the DualSense in pairing mode if no saved address exists.")
    print("Operate the DualSense during capture: sticks, buttons, triggers, and motion.")
    capture_result = capture_m61_hidp_log.main(capture_args)
    if capture_result != 0:
        return capture_result

    check_args = [
        str(args.output),
        "--min-reports",
        str(args.min_reports),
    ]
    if args.allow_connected_stream:
        check_args.append("--allow-connected-stream")
    else:
        check_args.append("--require-auto")
    if not args.no_require_security and not args.allow_connected_stream:
        check_args.append("--require-security")
    if not args.no_require_input_activity:
        check_args.append("--require-input-activity")
    if not args.basic_only:
        check_args.extend(["--require-hidp", "--require-full-report"])

    check_result = check_m61_hidp_log.main(check_args)
    if check_result != 0:
        return check_result

    if args.basic_only:
        print("M61 HIDP basic capture passed. This is not the full M61-only gate.")
    else:
        print("M61 HIDP hardware log gate passed.")
        print("Manual confirmation still required: pairing behavior, input mapping, latency, and reconnect.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
