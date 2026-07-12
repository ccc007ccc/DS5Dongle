#!/usr/bin/env python3
"""Check Ai-M61 DualSense HIDP probe runtime logs."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

REPORT_RE = re.compile(r"parsed report=0x([0-9a-fA-F]{2}) mode=([a-zA-Z]+)")
STATE_RE = re.compile(
    r"LX=\s*(?P<lx>\d+)\s+LY=\s*(?P<ly>\d+)\s+RX=\s*(?P<rx>\d+)\s+RY=\s*(?P<ry>\d+)\s+"
    r"L2=\s*(?P<l2>\d+)\s+R2=\s*(?P<r2>\d+)\s+dpad=(?P<dpad>\S+)\s+buttons=(?P<buttons>\S+)"
    r".*gyro=(?P<gyro>n/a|\([^)]+\)).*accel=(?P<accel>n/a|\([^)]+\))"
)


def parse_triplet(text: str) -> tuple[int, int, int] | None:
    if text == "n/a" or not (text.startswith("(") and text.endswith(")")):
        return None
    parts = text[1:-1].split(",")
    if len(parts) != 3:
        return None
    try:
        return (int(parts[0]), int(parts[1]), int(parts[2]))
    except ValueError:
        return None


def input_activity_failures(lines: list[str], require_motion: bool) -> list[str]:
    states: list[dict[str, object]] = []
    for line in lines:
        match = STATE_RE.search(line)
        if not match:
            continue
        states.append({
            "sticks": tuple(int(match.group(name)) for name in ("lx", "ly", "rx", "ry")),
            "triggers": (int(match.group("l2")), int(match.group("r2"))),
            "buttons": match.group("buttons"),
            "gyro": parse_triplet(match.group("gyro")),
            "accel": parse_triplet(match.group("accel")),
        })

    if not states:
        return ["no parsed state lines available for input activity check"]

    stick_samples = [state["sticks"] for state in states]
    trigger_samples = [state["triggers"] for state in states]
    stick_values = [value for sample in stick_samples for value in sample]  # type: ignore[union-attr]
    trigger_values = [value for sample in trigger_samples for value in sample]  # type: ignore[union-attr]
    buttons = [state["buttons"] for state in states]
    gyro_samples = [state["gyro"] for state in states if state["gyro"] is not None]
    accel_samples = [state["accel"] for state in states if state["accel"] is not None]

    stick_changed = any(len(set(axis)) > 1 for axis in zip(*stick_samples))
    accel_changed = any(len(set(axis)) > 1 for axis in zip(*accel_samples)) if accel_samples else False
    gyro_nonzero = any(value != 0 for sample in gyro_samples for value in sample)  # type: ignore[union-attr]

    failures: list[str] = []
    if not stick_changed and not any(value <= 80 or value >= 176 for value in stick_values):
        failures.append("input activity missing: move at least one stick during capture")
    if len(set(trigger_values)) <= 1 and not any(value >= 32 for value in trigger_values):
        failures.append("input activity missing: press L2 or R2 during capture")
    if not any(button != "none" for button in buttons):
        failures.append("input activity missing: press at least one button during capture")
    if require_motion and not (gyro_nonzero or accel_changed):
        failures.append("input activity missing: move the controller to exercise IMU fields")
    return failures


def contains_any(text: str, needles: list[str]) -> bool:
    return any(needle in text for needle in needles)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="Captured M61 HIDP probe log")
    parser.add_argument("--min-reports", type=int, default=1)
    parser.add_argument("--require-auto", action="store_true")
    parser.add_argument("--require-hidp", action="store_true")
    parser.add_argument("--require-security", action="store_true")
    parser.add_argument("--require-full-report", action="store_true")
    parser.add_argument("--require-input-activity", action="store_true")
    parser.add_argument(
        "--allow-connected-stream",
        action="store_true",
        help=(
            "accept logs captured after the controller was already connected; "
            "full parsed reports then prove the live HIDP stream even if boot/ACL "
            "banner lines are absent"
        ),
    )
    args = parser.parse_args(argv)

    if not args.log.is_file():
        parser.error(f"log file not found: {args.log}")

    text = args.log.read_text(encoding="utf-8", errors="replace")
    reports = REPORT_RE.findall(text)
    full_reports = [
        report_id for report_id, mode in reports
        if report_id.lower() == "31" and mode.lower() == "full"
    ]
    has_connected_stream = (
        args.allow_connected_stream
        and len(reports) >= args.min_reports
        and (bool(full_reports) or not args.require_full_report)
    )

    checks: list[tuple[str, bool]] = []
    if not has_connected_stream:
        checks.extend([
            (
                "Bluetooth stack reached ready callback",
                "M61 DualSense HIDP probe ready" in text,
            ),
            (
                "BR/EDR discovery or saved-address connect started",
                contains_any(text, [
                    "BR/EDR discovery started",
                    "auto: no saved DualSense address; scanning",
                    "auto: connecting saved DualSense",
                    "Connecting ",
                ]),
            ),
            (
                "BR/EDR ACL connected",
                "BR/EDR connected:" in text,
            ),
        ])

    checks.append((
        f"Parsed at least {args.min_reports} DualSense input report(s)",
        len(reports) >= args.min_reports,
    ))

    if args.require_auto:
        checks.extend([
            (
                "Auto task attempted connect or scan",
                contains_any(text, [
                    "auto: connecting saved DualSense",
                    "auto: no saved DualSense address; scanning",
                ]),
            ),
            (
                "Auto task attempted DualSense bring-up",
                "auto: DualSense bring-up attempt" in text,
            ),
        ])

    if args.require_security:
        checks.append((
            "BR/EDR security reached callback",
            "BR/EDR security changed:" in text,
        ))

    if args.require_hidp:
        checks.extend([
            (
                "HID Control L2CAP connected",
                "HIDP control connected:" in text,
            ),
            (
                "HID Interrupt L2CAP connected",
                "HIDP interrupt connected:" in text,
            ),
        ])

    if args.require_full_report:
        checks.append((
            "Full DualSense report=0x31 mode=full observed",
            bool(full_reports) or "M61 HIDP full report path is alive" in text,
        ))

    failed = [name for name, ok in checks if not ok]
    if args.require_input_activity:
        parsed_lines = [line for line in text.splitlines() if "parsed report=0x" in line]
        failed.extend(input_activity_failures(parsed_lines, bool(full_reports or args.require_full_report)))

    print("M61 HIDP log summary:")
    print(f"  parsed_reports={len(reports)}")
    print(f"  full_report_0x31={len(full_reports)}")
    print(f"  auto_bringup_attempts={text.count('auto: DualSense bring-up attempt')}")
    print(f"  hidp_control_connected={'yes' if 'HIDP control connected:' in text else 'no'}")
    print(f"  hidp_interrupt_connected={'yes' if 'HIDP interrupt connected:' in text else 'no'}")
    print(f"  connected_stream={'accepted' if has_connected_stream else 'not-used'}")
    print(f"  input_activity={'checked' if args.require_input_activity else 'not-checked'}")

    if failed:
        print("M61 HIDP log check failed:")
        for name in failed:
            print(f"  - {name}")
        return 1

    print("M61 HIDP log check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
