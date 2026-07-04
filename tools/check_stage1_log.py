#!/usr/bin/env python3
"""Check an ESP-IDF monitor log for stage-1 bring-up evidence.

The checker is intentionally conservative: it does not prove latency or packet
loss by itself, but it does verify that the log contains the required Classic
Bluetooth scan/connect or saved-address reconnect path and parsed DualSense
input reports. A basic 0x01 report proves the HID input path is alive;
--require-full-report is required for the final stage-1 gate because it checks
full 0x31 reports with motion/battery.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
TIMESTAMP_RE = re.compile(r"^[A-Z]\s+\((\d+)\)")
STATE_RE = re.compile(
    r"LX=\s*(?P<lx>\d+)\s+LY=\s*(?P<ly>\d+)\s+RX=\s*(?P<rx>\d+)\s+RY=\s*(?P<ry>\d+)\s+"
    r"L2=\s*(?P<l2>\d+)\s+R2=\s*(?P<r2>\d+)\s+dpad=(?P<dpad>\S+)\s+buttons=(?P<buttons>\S+)"
    r".*gyro=(?P<gyro>n/a|\([^)]+\)).*accel=(?P<accel>n/a|\([^)]+\))"
)
BASIC_REPORT_RE = re.compile(
    r"report=0x01\b.*\bLX=\s*\d+.*\bLY=\s*\d+.*\bRX=\s*\d+.*\bRY=\s*\d+"
    r".*\bL2=\s*\d+.*\bR2=\s*\d+.*\bdpad=.*\bbuttons=.*\bgyro=n/a"
)
FULL_REPORT_RE = re.compile(
    r"report=0x31\b.*\bLX=\s*\d+.*\bLY=\s*\d+.*\bRX=\s*\d+.*\bRY=\s*\d+"
    r".*\bL2=\s*\d+.*\bR2=\s*\d+.*\bdpad=.*\bbuttons=.*\bgyro=\("
    r".*\baccel=\(.*\bbattery=\d+%"
)


FAILURE_MARKERS = [
    "Guru Meditation",
    "abort()",
    "Backtrace:",
    "assert failed",
    "ESP_ERROR_CHECK failed",
    "esp_bt_controller_init failed",
    "esp_bt_controller_enable failed",
    "esp_bluedroid_init failed",
    "esp_bluedroid_enable failed",
    "esp_bt_hid_host_connect failed",
    "Raw HIDP L2CAP connect PSM",
    "esp_bt_l2cap_init failed",
]


FEATURE_REPORT_IDS = ["0x09", "0x20", "0x22", "0x05", "0x70"]
OUTPUT_INIT_MARKERS = [
    "DualSense bring-up attempt",
    "DS5Dongle report 0x31 DATA output sent",
    "DS5Dongle report 0x31 SET_REPORT output queued",
    "DS5Dongle report 0x32 DATA output sent",
    "DS5Dongle report 0x32 SET_REPORT output queued",
]


def load_lines(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    return [ANSI_RE.sub("", line.rstrip()) for line in text.splitlines()]


def timestamp_span_ms(lines: list[str]) -> int | None:
    values: list[int] = []
    for line in lines:
        match = TIMESTAMP_RE.match(line)
        if match:
            values.append(int(match.group(1)))
    if len(values) < 2:
        return None
    return max(values) - min(values)


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
        if "report=0x" not in line:
            continue
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
    gyro_samples: list[tuple[int, int, int]] = []
    accel_samples: list[tuple[int, int, int]] = []
    for state in states:
        gyro = state["gyro"]
        accel = state["accel"]
        if gyro is not None:
            gyro_samples.append(gyro)  # type: ignore[arg-type]
        if accel is not None:
            accel_samples.append(accel)  # type: ignore[arg-type]

    stick_changed = any(len(set(axis_values)) > 1 for axis_values in zip(*stick_samples))
    accel_changed = any(len(set(axis_values)) > 1 for axis_values in zip(*accel_samples)) if accel_samples else False
    gyro_nonzero = any(value != 0 for sample in gyro_samples for value in sample)

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


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="ESP-IDF monitor log captured during stage-1 testing")
    parser.add_argument(
        "--min-reports",
        type=int,
        default=3,
        help="minimum parsed DualSense input lines required",
    )
    parser.add_argument(
        "--min-duration-ms",
        type=int,
        default=0,
        help="minimum ESP-IDF timestamp span required; use 300000 for a 5 minute stability run",
    )
    parser.add_argument(
        "--allow-close",
        action="store_true",
        help="do not fail when HID close appears in the log",
    )
    parser.add_argument(
        "--require-full-report",
        action="store_true",
        help="require at least one full 0x31 report with motion and battery fields",
    )
    parser.add_argument(
        "--require-output-init",
        action="store_true",
        help="require DualSense feature requests and 0x31/0x32 output init logs",
    )
    parser.add_argument(
        "--require-input-activity",
        action="store_true",
        help="require evidence that sticks, triggers, buttons, and full-report motion were exercised",
    )
    args = parser.parse_args(argv)

    if not args.log.is_file():
        print(f"Stage 1 log check failed: missing log file {args.log}")
        return 1

    lines = load_lines(args.log)
    text = "\n".join(lines)
    failures: list[str] = []

    has_hidh_scan_path = (
        "Scanning for DualSense over Classic Bluetooth BR/EDR" in text and
        "Selected candidate" in text
    )
    has_raw_scan_path = (
        "Raw HIDP scanning for DualSense over Classic Bluetooth BR/EDR" in text and
        "Raw HIDP selected candidate" in text
    )
    has_hidh_saved_path = "Trying saved DualSense address" in text
    has_raw_saved_path = "Raw HIDP trying saved DualSense address" in text
    if not (has_hidh_scan_path or has_raw_scan_path or has_hidh_saved_path or has_raw_saved_path):
        failures.append("missing Bluetooth target path: no selected scan candidate or saved-address reconnect")

    has_hidh_open = (
        "HID open status=0" in text and
        "DualSense HID connected; requesting report protocol" in text
    )
    has_raw_open = (
        "Raw HIDP control connected:" in text and
        "Raw HIDP interrupt connected:" in text
    )
    if not (has_hidh_open or has_raw_open):
        failures.append("missing HID path opened: no HID Host open or raw HIDP Control/Interrupt channels")

    full_reports = [line for line in lines if FULL_REPORT_RE.search(line)]
    basic_reports = [line for line in lines if BASIC_REPORT_RE.search(line)]
    parsed_reports = full_reports + basic_reports
    if len(parsed_reports) < args.min_reports:
        failures.append(
            f"parsed report count {len(parsed_reports)} is below --min-reports {args.min_reports}"
        )
    if args.require_full_report and not full_reports:
        failures.append("no full 0x31 report found")

    if args.require_input_activity:
        failures.extend(input_activity_failures(parsed_reports, bool(full_reports or args.require_full_report)))

    if args.require_output_init:
        for report_id in FEATURE_REPORT_IDS:
            hidh_marker = f"Requested feature report {report_id}"
            raw_marker = f"Raw HIDP requested feature report {report_id}"
            if hidh_marker not in text and raw_marker not in text:
                failures.append(f"missing feature report request {report_id}")
        has_hidh_output = all(marker in text for marker in OUTPUT_INIT_MARKERS)
        has_raw_output = (
            "Raw HIDP DualSense bring-up attempt" in text and
            "Raw HIDP DS5Dongle report 0x31 DATA/SET_REPORT output attempt" in text and
            "Raw HIDP DS5Dongle report 0x32 DATA/SET_REPORT output attempt" in text
        )
        if not (has_hidh_output or has_raw_output):
            failures.append("missing output init markers for HID Host or raw HIDP backend")

    for marker in FAILURE_MARKERS:
        if marker in text:
            failures.append(f"failure marker found: {marker}")

    if not args.allow_close and "HID close" in text:
        failures.append("HID close found; rerun with --allow-close only for deliberate disconnect logs")

    unsupported_count = sum(
        "Unsupported HID interrupt report" in line or
        "Raw HIDP unsupported interrupt report" in line
        for line in lines
    )
    if unsupported_count > 10:
        failures.append(f"too many unsupported HID reports: {unsupported_count}")

    if args.min_duration_ms > 0:
        span = timestamp_span_ms(lines)
        if span is None:
            failures.append("cannot verify duration because ESP-IDF timestamps were not found")
        elif span < args.min_duration_ms:
            failures.append(
                f"log timestamp span {span} ms is below --min-duration-ms {args.min_duration_ms}"
            )

    if failures:
        print("Stage 1 log check failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        "Stage 1 log check passed "
        f"({len(full_reports)} full reports, {len(basic_reports)} basic reports, "
        f"unsupported={unsupported_count}, "
        f"output_init={'checked' if args.require_output_init else 'not-checked'}, "
        f"input_activity={'checked' if args.require_input_activity else 'not-checked'})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
