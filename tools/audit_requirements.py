#!/usr/bin/env python3
"""Audit the checkout against the current M61 DualSense USB adapter gates."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re

import check_m61_hidp_log


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SPEC = (
    Path.home()
    / ".codex"
    / "attachments"
    / "e710f4ea-e236-4654-af18-80e961383da9"
    / "pasted-text-1.txt"
)
USB_STATUS_RE = re.compile(
    r"usb_gamepad\s+ready=(?P<ready>[01])\s+configured=(?P<configured>[01])\s+"
    r"busy=(?P<busy>[01])\s+sent=(?P<sent>\d+)\s+dropped=(?P<dropped>\d+)"
)


@dataclass(frozen=True)
class AuditItem:
    status: str
    requirement: str
    evidence: str


def read_text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="ignore")


def path_exists(rel: str) -> bool:
    return (ROOT / rel).exists()


def contains_all(path: str, needles: list[str]) -> bool:
    text = read_text(path)
    return all(needle in text for needle in needles)


def add(items: list[AuditItem], status: str, requirement: str, evidence: str) -> None:
    items.append(AuditItem(status=status, requirement=requirement, evidence=evidence))


def audit_m61_log(log_path: Path | None) -> tuple[bool, str]:
    if log_path is None:
        return False, "no --m61-log was supplied"
    if not log_path.is_file():
        return False, f"{log_path} does not exist"

    args = [
        str(log_path),
        "--min-reports",
        "20",
        "--require-full-report",
        "--allow-connected-stream",
    ]
    result = check_m61_hidp_log.main(args)
    if result == 0:
        return True, f"{log_path} passed the M61 full-report log gate"
    return False, f"{log_path} did not pass the M61 full-report log gate"


def audit_usb_status(log_path: Path | None) -> tuple[bool, str]:
    if log_path is None:
        return False, "no --m61-log was supplied"
    if not log_path.is_file():
        return False, f"{log_path} does not exist"

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
        return True, f"{log_path} shows configured=1 and sent increased {sent_values[0]}->{sent_values[-1]}"
    return True, f"{log_path} shows configured=1 and sent={max_sent}"


def collect_audit(args: argparse.Namespace) -> list[AuditItem]:
    items: list[AuditItem] = []

    spec_path = Path(args.spec) if args.spec else DEFAULT_SPEC
    if spec_path.is_file():
        spec_text = spec_path.read_text(encoding="utf-8", errors="ignore")
        if "ESP32" in spec_text and "BL618" in spec_text:
            add(
                items,
                "PASS",
                "original pasted specification is readable",
                f"{spec_path}; current implementation intentionally moved to M61-first after hardware proof",
            )
        else:
            add(items, "FAIL", "original pasted specification is readable", "missing expected project terms")
    elif args.require_spec:
        add(items, "FAIL", "original pasted specification is readable", f"missing {spec_path}")
    else:
        add(items, "INFO", "original pasted specification is readable", f"not present at {spec_path}")

    legacy_paths = ["src", "boards", "lib", "cmake", "pico_sdk_import.cmake", ".gitmodules"]
    present_legacy = [path for path in legacy_paths if path_exists(path)]
    if present_legacy:
        add(items, "FAIL", "legacy Pico/TinyUSB paths stay removed", "still present: " + ", ".join(present_legacy))
    else:
        add(items, "PASS", "legacy Pico/TinyUSB paths stay removed", ", ".join(legacy_paths))

    if contains_all(
            "docs/PROJECT_STANDARD.md",
        [
            "DualSense --Classic Bluetooth HIDP--> M61 --USB DualSense composite--> PC",
            "ESP32 双芯片方案仍保留为 fallback",
            "USB_DP",
            "USB_DM",
            "usb_gamepad configured=1",
        ],
    ):
        add(items, "PASS", "project standard is M61-first and documents native USB constraints", "docs/PROJECT_STANDARD.md")
    else:
        add(items, "FAIL", "project standard is M61-first and documents native USB constraints", "missing standard markers")

    if contains_all(
        "m61/dualsense_hidp_probe/main.c",
        [
            "bt_conn_create_br",
            "bt_l2cap_chan_connect",
            "HIDP_PSM_CONTROL 0x0011",
            "HIDP_PSM_INTERRUPT 0x0013",
            "dualsense_parse_report",
            "m61_usb_gamepad_send_state",
        ],
    ):
        add(items, "PASS", "M61 Classic HIDP host path is implemented", "m61/dualsense_hidp_probe/main.c")
    else:
        add(items, "FAIL", "M61 Classic HIDP host path is implemented", "missing HIDP markers")

    if contains_all(
        "m61/dualsense_hidp_probe/m61_usb_gamepad.c",
        [
            "usbd_audio_init_intf",
            "usbd_hid_init_intf",
            "HID_DUALSENSE_REPORT_DESC_SIZE",
            "DualSense Wireless Controller",
            "AUDIO_OUT_PACKET_SIZE",
            "HID_IN_EP 0x84",
            "usbd_ep_start_write",
        ],
    ):
        add(items, "PASS", "M61 USB DualSense composite device is implemented", "m61/dualsense_hidp_probe/m61_usb_gamepad.c")
    else:
        add(items, "FAIL", "M61 USB DualSense composite device is implemented", "missing USB composite markers")

    if contains_all(
        "m61/dualsense_hidp_probe/defconfig",
        [
            "CONFIG_BT_BREDR =y",
            "CONFIG_CHERRYUSB_DEVICE =y",
            "CONFIG_CHERRYUSB_DEVICE_HID =y",
            "CONFIG_CHERRYUSB_DEVICE_AUDIO =y",
            "CONFIG_M61_USB_GAMEPAD_ENABLE =y",
        ],
    ):
        add(items, "PASS", "M61 firmware config enables BR/EDR and USB composite device", "m61/dualsense_hidp_probe/defconfig")
    else:
        add(items, "FAIL", "M61 firmware config enables BR/EDR and USB composite device", "missing defconfig markers")

    if contains_all(
        "m61/dualsense_hidp_probe/main.c",
        [
            "CONFIG_M61_STATUS_LED_RED_PIN",
            "CONFIG_M61_STATUS_LED_GREEN_PIN",
            "CONFIG_M61_STATUS_LED_BLUE_PIN",
            "STATUS_LED_CONNECTING",
            "STATUS_LED_CONNECTED",
        ],
    ):
        add(items, "PASS", "M61 status LED policy is implemented", "m61/dualsense_hidp_probe/main.c")
    else:
        add(items, "FAIL", "M61 status LED policy is implemented", "missing LED markers")

    log_ok, log_evidence = audit_m61_log(args.m61_log)
    add(
        items,
        "PASS" if log_ok else "PENDING",
        "M61 hardware log proves full DualSense report=0x31 input",
        log_evidence,
    )

    usb_ok, usb_evidence = audit_usb_status(args.m61_log)
    add(
        items,
        "PASS" if usb_ok else "PENDING",
        "PC enumerates M61 native USB DualSense composite device and accepts reports",
        usb_evidence,
    )

    add(
        items,
        "PENDING",
        "manual end-to-end validation covers mapping, latency, and 5 minute stability",
        "requires Windows/Linux gamepad tester evidence while using BL618 native USB_DP/USB_DM",
    )

    return items


def print_audit(items: list[AuditItem]) -> None:
    for item in items:
        print(f"{item.status}: {item.requirement}")
        print(f"  evidence: {item.evidence}")

    counts = {status: sum(1 for item in items if item.status == status) for status in ("PASS", "PENDING", "INFO", "FAIL")}
    print(
        "Audit summary: "
        f"pass={counts['PASS']} pending={counts['PENDING']} "
        f"info={counts['INFO']} fail={counts['FAIL']}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, help="path to the original pasted project specification")
    parser.add_argument("--require-spec", action="store_true", help="fail if the original spec file cannot be read")
    parser.add_argument("--m61-log", type=Path, help="real M61 HIDP/USB status log to check")
    parser.add_argument("--stage1-log", type=Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--strict-complete",
        action="store_true",
        help="return nonzero if any requirement is still pending",
    )
    args = parser.parse_args(argv)

    items = collect_audit(args)
    print_audit(items)

    if any(item.status == "FAIL" for item in items):
        return 1
    if args.strict_complete and any(item.status == "PENDING" for item in items):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
