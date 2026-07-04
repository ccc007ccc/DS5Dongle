#!/usr/bin/env python3
"""Self-test the stage-1 log checker with synthetic HIDH and raw HIDP logs."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile

import check_stage1_log


FEATURE_REPORT_IDS = ["0x09", "0x20", "0x22", "0x05", "0x70"]


FULL_REPORT_LINE = (
    "I (3000) ds5_test: report=0x31 mode=full seq=0x01 "
    "LX= 12 LY= 34 RX= 56 RY= 78 L2= 90 R2=123 dpad=SE buttons=cross "
    "gyro=(1,2,3) accel=(4,5,6) battery=50% power=0x2 hp=0 mic=0 muted=0"
)

IDLE_FULL_REPORT_LINE = (
    "I (3000) ds5_test: report=0x31 mode=full seq=0x01 "
    "LX=128 LY=128 RX=128 RY=128 L2=  0 R2=  0 dpad=idle buttons=none "
    "gyro=(0,0,0) accel=(0,0,0) battery=50% power=0x2 hp=0 mic=0 muted=0"
)

STATIC_IMU_FULL_REPORT_LINE = (
    "I (3000) ds5_test: report=0x31 mode=full seq=0x01 "
    "LX= 12 LY= 34 RX= 56 RY= 78 L2= 90 R2=123 dpad=SE buttons=cross "
    "gyro=(0,0,0) accel=(0,0,1024) battery=50% power=0x2 hp=0 mic=0 muted=0"
)


def run_checker(text: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "stage1.log"
        path.write_text(text, encoding="utf-8")
        return check_stage1_log.main([
            str(path),
            "--min-reports",
            "1",
            "--require-output-init",
            "--require-full-report",
            "--require-input-activity",
        ])


def make_hidh_log() -> str:
    return make_hidh_log_with_report(FULL_REPORT_LINE)


def make_idle_hidh_log() -> str:
    return make_hidh_log_with_report(IDLE_FULL_REPORT_LINE)


def make_static_imu_hidh_log() -> str:
    return make_hidh_log_with_report(STATIC_IMU_FULL_REPORT_LINE)


def make_hidh_log_with_report(report_line: str) -> str:
    feature_lines = "\n".join(
        f"I (2100) ds5_bt_host: Requested feature report {report_id} len=64"
        for report_id in FEATURE_REPORT_IDS
    )
    return "\n".join([
        "I (1000) ds5_bt_host: Scanning for DualSense over Classic Bluetooth BR/EDR",
        "I (1200) ds5_bt_host: Selected candidate A0:FA:9C:35:29:6F; stopping inquiry",
        "I (1800) ds5_bt_host: HID open status=0 conn=2 handle=1 addr=A0:FA:9C:35:29:6F",
        "I (1900) ds5_bt_host: DualSense HID connected; requesting report protocol",
        "I (2000) ds5_bt_host: DualSense bring-up attempt 1/8 reason=hid-open",
        feature_lines,
        "I (2200) ds5_bt_host: DS5Dongle report 0x31 DATA output sent seq=0",
        "I (2201) ds5_bt_host: DS5Dongle report 0x31 SET_REPORT output queued",
        "I (2202) ds5_bt_host: DS5Dongle report 0x32 DATA output sent seq=1",
        "I (2203) ds5_bt_host: DS5Dongle report 0x32 SET_REPORT output queued",
        report_line,
    ])


def make_raw_hidp_log() -> str:
    feature_lines = "\n".join(
        f"I (2100) ds5_raw_hidp: Raw HIDP requested feature report {report_id}"
        for report_id in FEATURE_REPORT_IDS
    )
    return "\n".join([
        "I (1000) ds5_raw_hidp: Raw HIDP trying saved DualSense address A0:FA:9C:35:29:6F",
        "I (1800) ds5_raw_hidp: Raw HIDP control connected: fd=3 tx_mtu=672",
        "I (1900) ds5_raw_hidp: Raw HIDP interrupt connected: fd=4 tx_mtu=672",
        "I (2000) ds5_raw_hidp: Raw HIDP DualSense bring-up attempt 1/8 reason=l2cap-open",
        feature_lines,
        "I (2200) ds5_raw_hidp: Raw HIDP DS5Dongle report 0x31 DATA/SET_REPORT output attempt",
        "I (2201) ds5_raw_hidp: Raw HIDP DS5Dongle report 0x32 DATA/SET_REPORT output attempt",
        FULL_REPORT_LINE,
    ])


def main() -> int:
    failures: list[str] = []
    if run_checker(make_hidh_log()) != 0:
        failures.append("synthetic HIDH log failed")
    if run_checker(make_raw_hidp_log()) != 0:
        failures.append("synthetic raw HIDP log failed")
    if run_checker(make_idle_hidh_log()) == 0:
        failures.append("synthetic idle log unexpectedly passed")
    if run_checker(make_static_imu_hidh_log()) == 0:
        failures.append("synthetic static IMU log unexpectedly passed")

    if failures:
        print("Stage 1 log checker self-test failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Stage 1 log checker self-test passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
