#!/usr/bin/env python3
"""Repository-level checks for the pure-M61 DualSense adapter."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


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
        "m61/FreeRTOSConfig.h",
        "m61/dualsense_hidp_probe/CMakeLists.txt",
        "m61/dualsense_hidp_probe/build.sh",
        "m61/dualsense_hidp_probe/defconfig",
        "m61/dualsense_hidp_probe/main.c",
        "m61/dualsense_hidp_probe/dualsense_parser.c",
        "m61/dualsense_hidp_probe/dualsense_parser.h",
        "m61/dualsense_hidp_probe/dualsense_output.c",
        "m61/dualsense_hidp_probe/dualsense_output.h",
        "m61/dualsense_hidp_probe/m61_usb_gamepad.c",
        "m61/dualsense_hidp_probe/m61_usb_gamepad.h",
        "tools/flash_m61_firmware.py",
        "tools/check_m61_hidp_log.py",
        "tools/check_m61_usb_windows.py",
        "tools/validate_m61_hidp_hardware.py",
        "tools/validate_m61_usb_hardware.py",
        "tools/run_offline_checks.py",
    ]
    forbidden_paths = [
        "CMakeLists.txt",
        "sdkconfig.defaults",
        "sdkconfig.raw_hidp.defaults",
        "main",
        "m61/esp32_prog_bridge",
        "tools/build_esp32_stage1.py",
        "tools/capture_stage1_log.py",
        "tools/check_stage1_log.py",
        "tools/flash_m61_bridge.py",
        "tools/flash_stage1_auto.py",
        "tools/flash_stage1_m61.py",
        "tools/flash_stage1_manual.py",
        "tools/m61_esp32_control.py",
        "tools/test_stage1_log_checker.py",
        "tools/validate_stage1_hardware.py",
        "docs/M61_DEBUG_BRIDGE.md",
        "docs/STAGE1_VALIDATION.md",
        ".gitmodules",
        "pico_sdk_import.cmake",
        "boards",
        "cmake",
        "lib",
        "src",
    ]

    for path in required_files:
        if not (ROOT / path).is_file():
            failures.append(f"missing required file: {path}")
    for path in forbidden_paths:
        if (ROOT / path).exists():
            failures.append(f"non-M61 path must not exist: {path}")

    cmake = (ROOT / "m61/dualsense_hidp_probe/CMakeLists.txt").read_text(encoding="utf-8")
    for source in ("dualsense_output.c", "dualsense_parser.c", "m61_usb_gamepad.c"):
        if f"target_sources(app PRIVATE {source})" not in cmake:
            failures.append(f"M61 CMake must compile local source: {source}")
    if "../../main" in cmake:
        failures.append("M61 CMake must not reference the deleted root main directory")

    standard = (ROOT / "docs/PROJECT_STANDARD.md").read_text(encoding="utf-8")
    for marker in (
        "DualSense --Classic Bluetooth HIDP--> M61 --USB DualSense composite--> PC",
        "仓库只保留 M61 实现",
        "BL618 的 `USB_DP`/`USB_DM`",
    ):
        if marker not in standard:
            failures.append(f"project standard missing marker: {marker}")

    for path in ROOT.rglob("*.ino"):
        failures.append(f"MCU firmware must not use Arduino .ino file: {path.relative_to(ROOT)}")

    if failures:
        print("Project verification failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Project verification passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
