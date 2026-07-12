#!/usr/bin/env python3
"""Self-test the current M61 requirements audit command."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile

import audit_requirements


REPORT_LINE = (
    "parsed report=0x31 mode=full seq=0x01 LX= 12 LY= 34 RX= 56 RY= 78 "
    "L2= 90 R2=123 dpad=SE buttons=cross gyro=(1,2,3) accel=(4,5,6) "
    "battery=50% power=0x2 hp=0 mic=0 muted=0"
)


def write_spec(tmp: Path) -> Path:
    spec = tmp / "pasted-text-1.txt"
    spec.write_text(
        "ESP32 UART2 GPIO16 GPIO17 BL618 DualSense\n",
        encoding="utf-8",
    )
    return spec


def write_m61_log(tmp: Path, usb_configured: bool, sent_count: int = 20, name: str = "m61.log") -> Path:
    lines = [
        "M61 DualSense HIDP probe ready. Use 'ds5 scan'.",
        "BR/EDR connected: A0:FA:9C:35:29:6F",
        "HIDP control connected: rx_cid=0x0040 tx_cid=0x0041 rx_mtu=672 tx_mtu=672",
        "HIDP interrupt connected: rx_cid=0x0042 tx_cid=0x0043 rx_mtu=672 tx_mtu=672",
    ]
    for index in range(20):
        lines.append(REPORT_LINE.replace("seq=0x01", f"seq=0x{index & 0x0f:X}1"))
    lines.append(
        "usb_gamepad ready=1 configured=%d busy=0 sent=%d dropped=0"
        % (1 if usb_configured else 0, sent_count)
    )
    path = tmp / name
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def find_item(items: list[audit_requirements.AuditItem], requirement: str) -> audit_requirements.AuditItem:
    for item in items:
        if item.requirement == requirement:
            return item
    raise AssertionError(f"missing audit item: {requirement}")


def main() -> int:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        spec = write_spec(tmp)
        log = write_m61_log(tmp, usb_configured=True)
        configured_no_sent_log = write_m61_log(
            tmp,
            usb_configured=True,
            sent_count=0,
            name="m61_configured_no_sent.log",
        )

        if audit_requirements.main(["--spec", str(spec)]) != 0:
            failures.append("non-strict audit with no log should pass with pending items")
        if audit_requirements.main(["--spec", str(spec), "--strict-complete"]) != 2:
            failures.append("strict audit with no log should return pending exit code 2")

        args = type("Args", (), {
            "spec": spec,
            "require_spec": True,
            "m61_log": log,
        })()
        items = audit_requirements.collect_audit(args)
        m61_gate = find_item(items, "M61 hardware log proves full DualSense report=0x31 input")
        usb_gate = find_item(items, "PC enumerates M61 native USB DualSense composite device and accepts reports")
        manual_gate = find_item(items, "manual end-to-end validation covers mapping, latency, and 5 minute stability")
        if m61_gate.status != "PASS":
            failures.append("synthetic M61 full-report log should satisfy the M61 audit item")
        if usb_gate.status != "PASS":
            failures.append("synthetic usb_gamepad configured=1 sent>0 line should satisfy the USB audit item")
        args_no_sent = type("Args", (), {
            "spec": spec,
            "require_spec": True,
            "m61_log": configured_no_sent_log,
        })()
        no_sent_items = audit_requirements.collect_audit(args_no_sent)
        no_sent_usb_gate = find_item(
            no_sent_items,
            "PC enumerates M61 native USB DualSense composite device and accepts reports",
        )
        if no_sent_usb_gate.status != "PENDING":
            failures.append("configured=1 with sent=0 must not satisfy the USB audit item")
        if manual_gate.status != "PENDING":
            failures.append("manual stability gate must remain pending")
        if audit_requirements.main([
            "--spec",
            str(spec),
            "--m61-log",
            str(log),
            "--strict-complete",
        ]) != 2:
            failures.append("strict audit with only synthetic log evidence should still return pending exit code 2")

    if failures:
        print("Requirements audit self-test failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Requirements audit self-test passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
