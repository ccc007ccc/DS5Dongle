#!/usr/bin/env python3
"""Validate the M61 native USB DualSense composite path on connected hardware."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import tempfile

import check_m61_usb_windows


SAMPLE_M61_PNP_JSON = r"""[
  {
    "Status": "OK",
    "Class": "USB",
    "FriendlyName": "USB Composite Device",
    "InstanceId": "USB\\VID_054C&PID_0CE6\\M61DS5COMPOSITE1"
  },
  {
    "Status": "OK",
    "Class": "HIDClass",
    "FriendlyName": "HID-compliant game controller",
    "InstanceId": "HID\\VID_054C&PID_0CE6&MI_03\\0001"
  },
  {
    "Status": "OK",
    "Class": "MEDIA",
    "FriendlyName": "USB Audio Device",
    "InstanceId": "USB\\VID_054C&PID_0CE6&MI_00\\0001"
  }
]"""

SAMPLE_USB_STATUS_LOG = "\n".join([
    "hidp report log=quiet",
    "bt_ready=1 pending=0 connected=1 hid_control=1 hid_interrupt=1 have_last=1",
    "auto=1 sequence=1 security=1 sdp=1 hidp=1 full_report=1 bringup=1/8",
    "usb_gamepad ready=1 configured=1 busy=0 sent=42 dropped=0",
    "usb_audio open=2 close=0 out_open=1 in_open=1 last_open=2 last_close=0 out_pkts=12 out_bytes=4704 in_pkts=12 in_bytes=2352",
    "hidp_reports parsed=42 full=42 mic_audio=0 log=quiet",
])

USB_STATUS_RE = re.compile(
    r"usb_gamepad\s+ready=(?P<ready>[01])\s+configured=(?P<configured>[01])\s+"
    r"busy=(?P<busy>[01])\s+sent=(?P<sent>\d+)\s+dropped=(?P<dropped>\d+)"
)


def classify_windows(sample_json: Path | None) -> tuple[bool, str]:
    try:
        raw = sample_json.read_text(encoding="utf-8") if sample_json else check_m61_usb_windows.pnp_query_json()
        classification = check_m61_usb_windows.classify_devices(check_m61_usb_windows.parse_devices(raw))
    except Exception as exc:
        return False, f"Windows USB PnP query failed: {exc}"

    check_m61_usb_windows.print_report(classification)
    if classification.found_m61:
        return True, "Windows sees VID_054C&PID_0CE6 / DualSense Wireless Controller"
    return False, "Windows does not see the M61 native USB DualSense composite device"


def capture_usb_status(port: str, baud: int, output: Path, duration: int) -> int:
    # Keep pyserial optional for fixture-only/offline validation.
    import capture_m61_hidp_log

    return capture_m61_hidp_log.main([
        "-p",
        port,
        "-b",
        str(baud),
        "-o",
        str(output),
        "--duration",
        str(duration),
        "--usb-status",
        "--no-stdout",
    ])


def check_usb_status(log_path: Path) -> tuple[bool, str]:
    text = log_path.read_text(encoding="utf-8", errors="ignore")
    matches = list(USB_STATUS_RE.finditer(text))
    if not matches:
        return False, f"{log_path} does not contain a parseable usb_gamepad status line"

    configured = [match for match in matches if match.group("configured") == "1"]
    if not configured:
        return False, f"{log_path} does not show usb_gamepad configured=1"

    sent_values = [int(match.group("sent")) for match in configured]
    max_sent = max(sent_values)
    if max_sent <= 0:
        return False, f"{log_path} shows configured=1 but no USB reports sent yet"
    if len(sent_values) >= 2 and sent_values[-1] > sent_values[0]:
        return True, (
            f"{log_path} shows configured=1 and sent increased "
            f"{sent_values[0]}->{sent_values[-1]}"
        )
    return True, f"{log_path} shows configured=1 and sent={max_sent}"


def self_test() -> int:
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        pnp_json = tmp / "pnp.json"
        pnp_json.write_text(SAMPLE_M61_PNP_JSON, encoding="utf-8")
        status_log = tmp / "m61_usb_status.log"
        status_log.write_text(SAMPLE_USB_STATUS_LOG, encoding="utf-8")

        windows_ok, windows_evidence = classify_windows(pnp_json)
        status_ok, status_evidence = check_usb_status(status_log)

    if not windows_ok or not status_ok:
        print("M61 USB hardware validator self-test failed:")
        print(f"  windows_ok={windows_ok}: {windows_evidence}")
        print(f"  status_ok={status_ok}: {status_evidence}")
        return 1

    print("M61 USB hardware validator self-test passed.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", default="COM5", help="M61 CH340 console port")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="M61 console baud")
    parser.add_argument("-o", "--output", type=Path, default=Path("m61_usb_status.log"))
    parser.add_argument("--duration", type=int, default=3, help="USB status capture duration in seconds")
    parser.add_argument("--skip-windows", action="store_true", help="skip Windows PnP enumeration check")
    parser.add_argument("--sample-pnp-json", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--self-test", action="store_true", help="run offline parser self-test")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    windows_ok = True
    if not args.skip_windows:
        windows_ok, windows_evidence = classify_windows(args.sample_pnp_json)
        print(f"windows_usb: {'PASS' if windows_ok else 'PENDING'}: {windows_evidence}")

    print("Capturing M61 USB status over serial...")
    capture_result = capture_usb_status(args.port, args.baud, args.output, args.duration)
    if capture_result != 0:
        return capture_result

    status_ok, status_evidence = check_usb_status(args.output)
    print(f"m61_usb_status: {'PASS' if status_ok else 'PENDING'}: {status_evidence}")
    if not status_ok and "parseable usb_gamepad status line" in status_evidence:
        print("Hint: flash the latest m61/dualsense_hidp_probe firmware, then retry.")
        print("      The validator expects 'ds5 status' to print usb_gamepad ready/configured/sent.")

    if windows_ok and status_ok:
        print("M61 native USB DualSense composite hardware gate passed.")
        print("Manual gamepad tester and 5 minute stability validation are still required.")
        return 0

    print("M61 native USB DualSense composite hardware gate is still pending.")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
