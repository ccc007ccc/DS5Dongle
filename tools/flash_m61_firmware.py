#!/usr/bin/env python3
"""Flash one of the DS5Dongle M61 firmware images with Bouffalo tools."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
import time

import serial


ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = ROOT.parent / "bl_mcu_sdk"
FLASH_TOOL = SDK_ROOT / "tools" / "bflb_tools" / "bouffalo_flash_cube" / "BLFlashCommand.exe"
REBOOT_ISP_COMMANDS = (
    "~m61 reboot-isp",
    "ds5 reboot-isp",
    "m61 reboot-isp",
    "reboot uart",
)
REBOOT_ISP_BAUDS = (115200, 2000000, 460800)


@dataclass(frozen=True)
class FirmwareApp:
    directory: Path
    bl616_bin: str
    build_command: str


FIRMWARE_APPS = {
    "bridge": FirmwareApp(
        directory=ROOT / "m61" / "esp32_prog_bridge",
        bl616_bin="m61_esp32_prog_bridge_bl616.bin",
        build_command="wsl bash /mnt/c/code/MCU/DS5Dongle/m61/esp32_prog_bridge/build.sh",
    ),
    "hidp-probe": FirmwareApp(
        directory=ROOT / "m61" / "dualsense_hidp_probe",
        bl616_bin="m61_dualsense_hidp_probe_bl616.bin",
        build_command="wsl bash /mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh",
    ),
    "usb-ram-disk-probe": FirmwareApp(
        directory=ROOT / "m61" / "usb_ram_disk_probe",
        bl616_bin="m61_usb_ram_disk_probe_bl616.bin",
        build_command="wsl bash /mnt/c/code/MCU/DS5Dongle/m61/usb_ram_disk_probe/build.sh",
    ),
    "usb-hid-gamepad-probe": FirmwareApp(
        directory=ROOT / "m61" / "usb_hid_gamepad_probe",
        bl616_bin="m61_usb_hid_gamepad_probe_bl616.bin",
        build_command="wsl bash /mnt/c/code/MCU/DS5Dongle/m61/usb_hid_gamepad_probe/build.sh",
    ),
}


def read_pending(ser: serial.Serial, timeout_ms: int) -> bytes:
    deadline = time.monotonic() + timeout_ms / 1000.0
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = ser.read(4096)
        if data:
            chunks.append(data)
        else:
            time.sleep(0.02)
    return b"".join(chunks)


def print_text_lossy(text: str) -> None:
    encoding = sys.stdout.encoding or "utf-8"
    print(text.encode(encoding, errors="replace").decode(encoding))


def try_reboot_isp(port: str, baud: int, wait_ms: int) -> bool:
    print(f"requesting M61 UART download reboot on {port} @ {baud}")
    try:
        with serial.Serial(port, baudrate=baud, timeout=0.05, rtscts=False, dsrdtr=False) as ser:
            ser.setDTR(False)
            ser.setRTS(False)
            time.sleep(0.05)
            ser.reset_input_buffer()

            for command in REBOOT_ISP_COMMANDS:
                try:
                    ser.write(command.encode("ascii") + b"\r\n")
                    ser.flush()
                except serial.SerialException as exc:
                    print(f"serial write stopped after '{command}': {exc}")
                    break
                time.sleep(0.08)

            output = read_pending(ser, 300)
            if output:
                preview = output.decode("utf-8", errors="replace").strip()
                if preview:
                    print_text_lossy(preview)
    except serial.SerialException as exc:
        print(f"warning: unable to request M61 reboot to ISP at {baud}: {exc}", file=sys.stderr)
        return False

    time.sleep(wait_ms / 1000.0)
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", choices=sorted(FIRMWARE_APPS), default="bridge", help="M61 firmware app")
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
    parser.add_argument(
        "--no-reset",
        action="store_true",
        help="do not ask BLFlashCommand to reset M61 after programming",
    )
    args = parser.parse_args(argv)

    app = FIRMWARE_APPS[args.app]
    flash_config = app.directory / "flash_prog_cfg.ini"

    if not FLASH_TOOL.is_file():
        print(f"missing BLFlashCommand.exe: {FLASH_TOOL}", file=sys.stderr)
        return 1
    if not flash_config.is_file():
        print(f"missing flash config: {flash_config}", file=sys.stderr)
        return 1

    if args.chip == "bl616":
        firmware = app.directory / "build" / "build_out" / app.bl616_bin
        if not firmware.is_file():
            print(f"missing firmware: {firmware}", file=sys.stderr)
            print(f"run: {app.build_command}", file=sys.stderr)
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
    if not args.no_reset:
        cmd.append("--reset")

    print("running:", " ".join(cmd))
    return subprocess.call(cmd, cwd=app.directory)


if __name__ == "__main__":
    raise SystemExit(main())
