#!/usr/bin/env python3
"""Repository-level checks for the dual-chip DS5Dongle layout.

ESP32 (BTstack Classic host) + M61/BL618 (USB composite) joined by the
SPI frame protocol. Verifies required files exist and key invariants hold.
"""

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
    if not (ROOT / path).is_file():
        failures.append(f"missing required file: {path}")
        return
    text = read_text(path)
    for needle in needles:
        require(needle in text, f"{path} must contain: {needle}", failures)


def main() -> int:
    failures: list[str] = []

    required_files = [
        "README.md",
        "docs/REBUILD_PLAN.md",
        "docs/DUAL_CHIP_WIRING.md",
        "docs/DUALSENSE_REPORT_31.md",
        "docs/M61_NATIVE_USB_WIRING.md",
        # ESP32 side
        "main/app_main.c",
        "main/bt_ds5_btstack.c",
        "main/bt_dualsense_raw_hidp.h",
        "main/dual_chip_spi_proto.c",
        "main/dual_chip_spi_proto.h",
        "main/esp32_dual_chip_spi.c",
        "main/dualsense_parser.c",
        "main/dualsense_output.c",
        "components/btstack/CMakeLists.txt",
        "components/btstack/include/btstack_config.h",
        "components/btstack/port/btstack_port_esp32.c",
        "sdkconfig.dual_chip.defaults",
        "sdkconfig.dual_chip.devkit_left.defaults",
        # M61 side
        "m61/dualsense_hidp_probe/CMakeLists.txt",
        "m61/dualsense_hidp_probe/Makefile",
        "m61/dualsense_hidp_probe/defconfig",
        "m61/dualsense_hidp_probe/defconfig.dual_chip",
        "m61/dualsense_hidp_probe/main.c",
        "m61/dualsense_hidp_probe/m61_usb_gamepad.c",
        "m61/dualsense_hidp_probe/m61_esp32_transport.c",
        "m61/dualsense_hidp_probe/m61_ds5_bridge_config.c",
        # tooling
        "tools/build_esp32_stage1.py",
        "tools/build_m61.sh",
        "tools/run_offline_checks.py",
    ]
    for path in required_files:
        require_file(path, failures)

    # bluedroid legacy must stay gone
    forbidden_paths = [
        "main/bt_dualsense_host.c",
        "main/bt_dualsense_raw_hidp.c",
        "tools/patch_esp_idf_hidp_l2cap.py",
    ]
    for path in forbidden_paths:
        require(not (ROOT / path).exists(),
                f"legacy bluedroid path must not exist: {path}", failures)

    require_contains(
        "sdkconfig.dual_chip.defaults",
        [
            "CONFIG_BT_CONTROLLER_ONLY=y",
            "CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y",
            "CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR=y",
        ],
        failures,
    )
    require_contains(
        "main/bt_ds5_btstack.c",
        [
            "PSM_HID_CONTROL",
            "PSM_HID_INTERRUPT",
            "DS5_CRC_SEED_OUTPUT 0xEADA2D49",
            "DS5_CRC_SEED_FEATURE 0x2060EFC3",
            "gap_delete_all_link_keys",
        ],
        failures,
    )
    require_contains(
        "m61/dualsense_hidp_probe/defconfig.dual_chip",
        [
            "CONFIG_M61_DS5_DUAL_CHIP_TRANSPORT =y",
            "CONFIG_M61_ESP32_SPI_ENABLE =y",
        ],
        failures,
    )
    require_contains(
        "README.md",
        [
            "DS5Dongle",
            "awalol/DS5Dongle",
            "components/btstack",
            "tools/build_m61.sh",
        ],
        failures,
    )

    if failures:
        print("project structure check failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("project structure check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
