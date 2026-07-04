#!/usr/bin/env python3
"""Repository-level checks for the current M61 DualSense USB adapter standard."""

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def read_text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8", errors="ignore")


def require(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def require_file(path: str, failures: list[str]) -> None:
    require((ROOT / path).is_file(), f"missing required file: {path}", failures)


def require_contains(path: str, needles: list[str], failures: list[str]) -> None:
    text = read_text(path)
    for needle in needles:
        require(needle in text, f"{path} must contain: {needle}", failures)


def main() -> int:
    failures: list[str] = []

    required_files = [
        "README.md",
        "docs/PROJECT_STANDARD.md",
        "docs/IMPLEMENTATION_STATUS.md",
        "docs/DUALSENSE_REPORT_31.md",
        "docs/M61_BLUETOOTH_CAPABILITY.md",
        "docs/M61_NATIVE_USB_WIRING.md",
        "docs/WAKEUP_RUNBOOK.md",
        "m61/dualsense_hidp_probe/CMakeLists.txt",
        "m61/dualsense_hidp_probe/Makefile",
        "m61/dualsense_hidp_probe/build.sh",
        "m61/dualsense_hidp_probe/defconfig",
        "m61/dualsense_hidp_probe/flash_prog_cfg.ini",
        "m61/dualsense_hidp_probe/main.c",
        "m61/dualsense_hidp_probe/m61_usb_gamepad.c",
        "m61/dualsense_hidp_probe/m61_usb_gamepad.h",
        "m61/dualsense_hidp_probe/usb_config.h",
        "main/dualsense_parser.c",
        "main/dualsense_parser.h",
        "main/dualsense_output.c",
        "main/dualsense_output.h",
        "tools/flash_m61_firmware.py",
        "tools/capture_m61_hidp_log.py",
        "tools/check_m61_hidp_log.py",
        "tools/check_m61_usb_windows.py",
        "tools/validate_m61_hidp_hardware.py",
        "tools/validate_m61_usb_hardware.py",
        "tools/test_m61_hidp_log_checker.py",
        "tools/run_offline_checks.py",
    ]
    for path in required_files:
        require_file(path, failures)

    forbidden_paths = [
        ".gitmodules",
        "pico_sdk_import.cmake",
        "README.CN.md",
        "boards",
        "cmake",
        "lib",
        "src",
    ]
    for path in forbidden_paths:
        require(not (ROOT / path).exists(), f"legacy Pico path must not exist: {path}", failures)

    require_contains(
        "CMakeLists.txt",
        [
            "fallback",
            "m61/dualsense_hidp_probe",
            "project(ds5_dualsense_bridge_esp32)",
        ],
        failures,
    )
    require_contains(
        "docs/PROJECT_STANDARD.md",
        [
            "DualSense --Classic Bluetooth HIDP--> M61 --USB HID Gamepad--> PC",
            "ESP32 双芯片方案仍保留为 fallback",
            "BL618 的 `USB_DP`/`USB_DM`",
            "只接 CH340",
            "GPIO12",
            "GPIO14",
            "GPIO15",
            "usb_gamepad configured=1",
            "tools/check_m61_usb_windows.py",
            "tools/validate_m61_usb_hardware.py",
            "docs/M61_NATIVE_USB_WIRING.md",
        ],
        failures,
    )
    require_contains(
        "README.md",
        [
            "M61 DualSense USB Adapter",
            "M61 直接通过 Classic Bluetooth HIDP",
            "USB HID Gamepad Device",
            "板载 CH340 串口不会因为固件变成手柄",
            "不能直接接 Ai-M61 模组 `VCC`",
            "docs/M61_NATIVE_USB_WIRING.md",
            "--allow-connected-stream",
            "--usb-status",
            "tools\\check_m61_usb_windows.py",
            "tools\\validate_m61_usb_hardware.py",
        ],
        failures,
    )
    require_contains(
        "docs/DUALSENSE_REPORT_31.md",
        [
            "共享",
            "M61",
            "report=0x31 mode=full",
            "report=0x01 mode=basic",
        ],
        failures,
    )
    require(
        "最终阶段 1" not in read_text("docs/DUALSENSE_REPORT_31.md"),
        "docs/DUALSENSE_REPORT_31.md must not contain stale phrase: 最终阶段 1",
        failures,
    )
    require_contains(
        "docs/REQUIREMENTS_AUDIT.md",
        [
            "M61-first",
            "M61 USB HID Gamepad",
            "BL618 原生 `USB_DP/USB_DM`",
            "ESP32 双芯片方案作为 fallback",
        ],
        failures,
    )
    require_contains(
        "docs/SPEC_ALIGNMENT.md",
        [
            "规格演进和当前对齐",
            "M61 是当前默认主线",
            "原先“必须先 ESP32，BL618 只做 USB”的阶段门槛被后续 M61-only 指令覆盖",
            "CH340 串口口不能通过固件变成手柄",
        ],
        failures,
    )
    require_contains(
        "docs/WAKEUP_RUNBOOK.md",
        [
            "M61-only 是默认主线",
            "USB 线绿色 D+  -> M61 USB_DP",
            "CH340 口只提供串口/刷写",
            "python tools\\check_m61_usb_windows.py",
            "usb_gamepad ready=<0|1> configured=<0|1>",
            "ds5 log quiet",
        ],
        failures,
    )
    require_contains(
        "docs/M61_BLUETOOTH_CAPABILITY.md",
        [
            "M61-only 路线已经从“可行性探针”推进为当前主线",
            "USB HID Gamepad 已加入固件",
            "VID/PID：`1209:5D51`",
            "configured=1",
        ],
        failures,
    )
    require_contains(
        "docs/M61_NATIVE_USB_WIRING.md",
        [
            "USB_DP` | D+",
            "USB_DM` | D-",
            "VID_1209&PID_5D51",
            "VID_0000&PID_0002",
            "不要把 USB 5V 直接接 Ai-M61 模组 `VCC`",
            "python tools\\validate_m61_usb_hardware.py -p COM5",
        ],
        failures,
    )
    require_contains(
        "docs/STAGE1_VALIDATION.md",
        [
            "ESP32 fallback stage-1 validation",
            "只用于 ESP32 fallback 路线",
            "该路线不会解决 M61 当前 USB 不枚举的问题",
        ],
        failures,
    )
    require_contains(
        "docs/M61_DEBUG_BRIDGE.md",
        [
            "M61 ESP32 fallback 调试桥",
            "M61 ESP32 调试桥只用于刷写/复位 ESP32",
            "当前 M61 HIDP+USB 固件在 `m61/dualsense_hidp_probe`",
        ],
        failures,
    )
    require_contains(
        "m61/dualsense_hidp_probe/defconfig",
        [
            "CONFIG_BTBLECONTROLLER_LIB =ble1m2s1bredr1",
            "CONFIG_BT_BREDR =y",
            "CONFIG_CHERRYUSB_DEVICE =y",
            "CONFIG_CHERRYUSB_DEVICE_HID =y",
            "CONFIG_M61_STATUS_LED_RED_PIN =12",
            "CONFIG_M61_STATUS_LED_GREEN_PIN =14",
            "CONFIG_M61_STATUS_LED_BLUE_PIN =15",
            "CONFIG_M61_USB_GAMEPAD_ENABLE =y",
        ],
        failures,
    )
    require_contains(
        "m61/dualsense_hidp_probe/CMakeLists.txt",
        [
            "m61_usb_gamepad.c",
            "../../main/dualsense_output.c",
            "../../main/dualsense_parser.c",
        ],
        failures,
    )
    require_contains(
        "m61/dualsense_hidp_probe/main.c",
        [
            '#include "m61_usb_gamepad.h"',
            "bt_conn_create_br",
            "bt_l2cap_chan_connect",
            "HIDP_PSM_CONTROL 0x0011",
            "HIDP_PSM_INTERRUPT 0x0013",
            "dualsense_parse_report",
            "m61_usb_gamepad_send_state",
            "usb_gamepad ready=",
            "hidp_reports parsed=",
            "M61 HIDP full report path is alive",
            "m61 led",
            "ds5 reboot-isp",
            "ds5 log",
        ],
        failures,
    )
    require_contains(
        "m61/dualsense_hidp_probe/m61_usb_gamepad.c",
        [
            "usbd_hid_init_intf",
            "M61 DualSense Gamepad",
            "M61 DualSense Gamepad",
            "HID_GAMEPAD_REPORT_SIZE 9",
            "map_buttons",
            "m61_usb_gamepad_send_state",
            "usbd_ep_start_write",
            "usbd_hid_get_report",
        ],
        failures,
    )
    require_contains(
        "m61/dualsense_hidp_probe/usb_config.h",
        [
            "CONFIG_USBDEV_MAX_BUS",
            "CONFIG_USBHOST_MAX_ENDPOINTS",
            "CONFIG_USB_MUSB_EP_NUM",
            "CONFIG_USB_HS",
        ],
        failures,
    )
    require_contains(
        "tools/check_m61_hidp_log.py",
        [
            "--allow-connected-stream",
            "connected_stream",
            "--require-full-report",
            "--require-input-activity",
        ],
        failures,
    )
    require_contains(
        "tools/check_m61_usb_windows.py",
        [
            "VID_1209",
            "PID_5D51",
            "VID_1A86",
            "PID_7523",
            "--self-test",
            "USB_DP -> USB D+",
        ],
        failures,
    )
    require_contains(
        "tools/validate_m61_hidp_hardware.py",
        [
            "--allow-connected-stream",
            "capture_m61_hidp_log.main",
            "check_m61_hidp_log.main",
        ],
        failures,
    )
    require_contains(
        "tools/validate_m61_usb_hardware.py",
        [
            "check_m61_usb_windows",
            "capture_m61_hidp_log.main",
            "audit_requirements.audit_usb_status",
            "--usb-status",
            "--no-stdout",
            "configured=1",
            "sent=42",
        ],
        failures,
    )

    for path in ROOT.rglob("*.ino"):
        failures.append(f"MCU firmware must not use Arduino .ino file: {path.relative_to(ROOT)}")

    forbidden_doc_phrases = [
        "阶段 1 主线仍是 ESP32",
        "在阶段 1 通过前，不实现正式",
        "最终产品里的 M61/BL618 USB HID Device 固件要等阶段 1",
        "M61-only HIDP 探针没有实机证明 `report=0x31 mode=full` 前，不能取消 ESP32 主线",
        "阶段 1 满足后，才能开始正式实现",
        "它不实现最终 USB HID 输出",
        "最终阶段 1",
    ]
    for path in sorted((ROOT / "docs").glob("*.md")):
        text = path.read_text(encoding="utf-8", errors="ignore")
        for phrase in forbidden_doc_phrases:
            if phrase in text:
                failures.append(f"{path.relative_to(ROOT)} contains stale project-standard phrase: {phrase}")

    if failures:
        print("Project verification failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Project verification passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
