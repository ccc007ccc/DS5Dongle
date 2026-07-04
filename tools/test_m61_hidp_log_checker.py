#!/usr/bin/env python3
"""Self-test the M61 HIDP probe log checker with a synthetic full-report log."""

from __future__ import annotations

from pathlib import Path
import tempfile
import sys

import check_m61_hidp_log


FULL_REPORT_LINE = (
    "parsed report=0x31 mode=full seq=0x01 LX= 12 LY= 34 RX= 56 RY= 78 "
    "L2= 90 R2=123 dpad=SE buttons=cross gyro=(1,2,3) accel=(4,5,6) "
    "battery=50% power=0x2 hp=0 mic=0 muted=0"
)

IDLE_FULL_REPORT_LINE = (
    "parsed report=0x31 mode=full seq=0x01 LX=128 LY=128 RX=128 RY=128 "
    "L2=  0 R2=  0 dpad=idle buttons=none gyro=(0,0,0) accel=(0,0,0) "
    "battery=50% power=0x2 hp=0 mic=0 muted=0"
)

STATIC_IMU_FULL_REPORT_LINE = (
    "parsed report=0x31 mode=full seq=0x01 LX= 12 LY= 34 RX= 56 RY= 78 "
    "L2= 90 R2=123 dpad=SE buttons=cross gyro=(0,0,0) accel=(0,0,1024) "
    "battery=50% power=0x2 hp=0 mic=0 muted=0"
)


def make_m61_hidp_log() -> str:
    return make_m61_hidp_log_with_report(FULL_REPORT_LINE)


def make_idle_m61_hidp_log() -> str:
    return make_m61_hidp_log_with_report(IDLE_FULL_REPORT_LINE)


def make_static_imu_m61_hidp_log() -> str:
    return make_m61_hidp_log_with_report(STATIC_IMU_FULL_REPORT_LINE)


def make_connected_stream_log() -> str:
    return "\n".join([
        FULL_REPORT_LINE.replace("seq=0x01", "seq=0x01"),
        FULL_REPORT_LINE.replace("seq=0x01", "seq=0x02").replace("LX= 12", "LX= 88"),
        FULL_REPORT_LINE.replace("seq=0x01", "seq=0x03").replace("R2=123", "R2=200"),
    ])


def make_m61_hidp_log_with_report(report_line: str) -> str:
    return "\n".join([
        "PHY RF init success",
        "M61 DualSense HIDP probe ready. Use 'ds5 scan'.",
        "auto: no saved DualSense address; scanning",
        "BR/EDR discovery started",
        "found addr=A0:FA:9C:35:29:6F class=0x002508 rssi=-42",
        "stored DualSense addr A0:FA:9C:35:29:6F",
        "Connecting A0:FA:9C:35:29:6F",
        "BR/EDR ACL create pending",
        "BR/EDR connected: A0:FA:9C:35:29:6F",
        "BR/EDR security changed: level=2 err=0",
        "SDP HID discover pending",
        "HIDP control connected: rx_cid=0x0040 tx_cid=0x0041 rx_mtu=672 tx_mtu=672",
        "HIDP interrupt connected: rx_cid=0x0042 tx_cid=0x0043 rx_mtu=672 tx_mtu=672",
        "auto: HIDP connect control=0 interrupt=0",
        "auto: DualSense bring-up attempt 1/8",
        report_line,
        "M61 HIDP full report path is alive",
    ])


def run_checker(text: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "m61_hidp.log"
        path.write_text(text, encoding="utf-8")
        return check_m61_hidp_log.main([
            str(path),
            "--min-reports",
            "1",
            "--require-auto",
            "--require-security",
            "--require-hidp",
            "--require-full-report",
            "--require-input-activity",
        ])


def run_connected_stream_checker(text: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "m61_connected.log"
        path.write_text(text, encoding="utf-8")
        return check_m61_hidp_log.main([
            str(path),
            "--min-reports",
            "3",
            "--require-full-report",
            "--allow-connected-stream",
        ])


def main() -> int:
    failures: list[str] = []
    if run_checker(make_m61_hidp_log()) != 0:
        failures.append("synthetic M61 HIDP log failed")
    if run_connected_stream_checker(make_connected_stream_log()) != 0:
        failures.append("synthetic already-connected M61 stream log failed")
    if run_checker(make_idle_m61_hidp_log()) == 0:
        failures.append("synthetic M61 idle log unexpectedly passed")
    if run_checker(make_static_imu_m61_hidp_log()) == 0:
        failures.append("synthetic M61 static IMU log unexpectedly passed")

    if failures:
        print("M61 HIDP log checker self-test failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("M61 HIDP log checker self-test passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
