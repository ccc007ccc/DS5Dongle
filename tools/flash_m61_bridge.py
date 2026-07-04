#!/usr/bin/env python3
"""Flash the M61 ESP32 programming bridge firmware with Bouffalo tools."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from flash_m61_firmware import REBOOT_ISP_BAUDS, try_reboot_isp


ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = ROOT.parent / "bl_mcu_sdk"
BRIDGE_DIR = ROOT / "m61" / "esp32_prog_bridge"
FLASH_TOOL = SDK_ROOT / "tools" / "bflb_tools" / "bouffalo_flash_cube" / "BLFlashCommand.exe"
FLASH_CONFIG = BRIDGE_DIR / "flash_prog_cfg.ini"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="M61 UART COM port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=460800, help="download baudrate")
    parser.add_argument("--chip", default="bl616", help="Bouffalo chip name")
    parser.add_argument(
        "--reboot-isp",
        action="store_true",
        help="ask a running M61 helper/default shell to reboot into UART download mode before flashing",
    )
    parser.add_argument(
        "--reboot-baud",
        action="append",
        type=int,
        help=(
            "baudrate to try for the pre-flash reboot command; repeatable. "
            f"If omitted, tries {', '.join(str(baud) for baud in REBOOT_ISP_BAUDS)}"
        ),
    )
    parser.add_argument(
        "--reboot-wait-ms",
        type=int,
        default=1200,
        help="delay after sending reboot-isp before starting BLFlashCommand",
    )
    parser.add_argument("--manual-hint", action="store_true", help="print manual boot instructions before flashing")
    args = parser.parse_args()

    if not FLASH_TOOL.is_file():
        print(f"missing BLFlashCommand.exe: {FLASH_TOOL}", file=sys.stderr)
        return 1
    if not FLASH_CONFIG.is_file():
        print(f"missing flash config: {FLASH_CONFIG}", file=sys.stderr)
        return 1

    firmware = BRIDGE_DIR / "build" / "build_out" / "m61_esp32_prog_bridge_bl616.bin"
    if args.chip == "bl616" and not firmware.is_file():
        print(f"missing firmware: {firmware}", file=sys.stderr)
        print("run: wsl bash /mnt/c/code/MCU/DS5Dongle/m61/esp32_prog_bridge/build.sh", file=sys.stderr)
        return 1

    if args.manual_hint:
        print("Put M61 into UART download mode now:")
        print("  1. hold BOOT")
        print("  2. press and release RESET/RST")
        print("  3. release BOOT")

    if args.reboot_isp:
        reboot_bauds = args.reboot_baud or list(REBOOT_ISP_BAUDS)
        for reboot_baud in reboot_bauds:
            try_reboot_isp(args.port, reboot_baud, args.reboot_wait_ms)

    cmd = [
        str(FLASH_TOOL),
        "--interface=uart",
        f"--baudrate={args.baud}",
        f"--port={args.port}",
        f"--chipname={args.chip}",
        "--config=flash_prog_cfg.ini",
    ]

    print("running:", " ".join(cmd))
    return subprocess.call(cmd, cwd=BRIDGE_DIR)


if __name__ == "__main__":
    raise SystemExit(main())
