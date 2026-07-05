#!/usr/bin/env python3
"""Check whether Windows can see the M61 DualSense composite USB device."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


DUALSENSE_VID_PID = ("VID_054C", "PID_0CE6")
CH340_VID_PID = ("VID_1A86", "PID_7523")
DESCRIPTOR_FAIL = ("VID_0000", "PID_0002")


@dataclass(frozen=True)
class UsbDevice:
    status: str
    device_class: str
    friendly_name: str
    instance_id: str


@dataclass(frozen=True)
class UsbClassification:
    dualsense_devices: list[UsbDevice]
    hid_devices: list[UsbDevice]
    audio_devices: list[UsbDevice]
    ch340_devices: list[UsbDevice]
    descriptor_failures: list[UsbDevice]
    matching_devices: list[UsbDevice]

    @property
    def found_m61(self) -> bool:
        return bool(self.dualsense_devices)

    @property
    def found_hid(self) -> bool:
        return bool(self.hid_devices)

    @property
    def found_audio(self) -> bool:
        return bool(self.audio_devices)


def _text(value: Any) -> str:
    return "" if value is None else str(value)


def parse_devices(raw_json: str) -> list[UsbDevice]:
    if not raw_json.strip():
        return []

    parsed = json.loads(raw_json)
    if parsed is None:
        return []
    if isinstance(parsed, dict):
        parsed = [parsed]
    if not isinstance(parsed, list):
        raise ValueError("PowerShell JSON output is not a device list")

    devices: list[UsbDevice] = []
    for item in parsed:
        if not isinstance(item, dict):
            continue
        devices.append(
            UsbDevice(
                status=_text(item.get("Status")),
                device_class=_text(item.get("Class")),
                friendly_name=_text(item.get("FriendlyName")),
                instance_id=_text(item.get("InstanceId")),
            )
        )
    return devices


def pnp_query_json() -> str:
    command = r"""
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$devices = Get-PnpDevice -PresentOnly | Where-Object {
  $_.InstanceId -match 'VID_054C|PID_0CE6|VID_1A86|PID_7523|VID_0000|PID_0002' -or
  $_.FriendlyName -match 'M61|DualSense|Wireless Controller|Gamepad|CH340|Unknown USB'
}
@($devices | Select-Object Status,Class,FriendlyName,InstanceId) | ConvertTo-Json -Compress
"""
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command", command],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "Get-PnpDevice failed")
    return result.stdout


def has_vid_pid(device: UsbDevice, vid_pid: tuple[str, str]) -> bool:
    haystack = f"{device.instance_id} {device.friendly_name}".upper()
    return all(part in haystack for part in vid_pid)


def classify_devices(devices: list[UsbDevice]) -> UsbClassification:
    matching: list[UsbDevice] = []
    dualsense: list[UsbDevice] = []
    hid: list[UsbDevice] = []
    audio: list[UsbDevice] = []
    ch340: list[UsbDevice] = []
    descriptor_failures: list[UsbDevice] = []

    for device in devices:
        upper = f"{device.instance_id} {device.friendly_name}".upper()
        is_ds = has_vid_pid(device, DUALSENSE_VID_PID) or "DUALSENSE WIRELESS CONTROLLER" in upper
        is_hid = is_ds and (device.device_class.upper() == "HIDCLASS" or "HID" in upper or "GAME CONTROLLER" in upper)
        is_audio = is_ds and (
            device.device_class.upper() in ("MEDIA", "AUDIOENDPOINT") or
            "MI_00" in upper or "MI_01" in upper or "MI_02" in upper or
            "AUDIO" in upper or "MICROPHONE" in upper or "SPEAKER" in upper
        )
        is_ch340 = has_vid_pid(device, CH340_VID_PID) or "CH340" in upper
        is_descriptor_failure = has_vid_pid(device, DESCRIPTOR_FAIL)

        if is_ds or is_ch340 or is_descriptor_failure:
            matching.append(device)
        if is_ds:
            dualsense.append(device)
        if is_hid:
            hid.append(device)
        if is_audio:
            audio.append(device)
        if is_ch340:
            ch340.append(device)
        if is_descriptor_failure:
            descriptor_failures.append(device)

    return UsbClassification(
        dualsense_devices=dualsense,
        hid_devices=hid,
        audio_devices=audio,
        ch340_devices=ch340,
        descriptor_failures=descriptor_failures,
        matching_devices=matching,
    )


def print_device(device: UsbDevice) -> None:
    print(f"  status={device.status} class={device.device_class}")
    print(f"  name={device.friendly_name}")
    print(f"  id={device.instance_id}")


def print_report(classification: UsbClassification) -> None:
    if classification.dualsense_devices:
        print("PASS: Windows sees VID_054C&PID_0CE6 / DualSense Wireless Controller.")
        for device in classification.dualsense_devices:
            print_device(device)
    else:
        print("PENDING: Windows does not see VID_054C&PID_0CE6 / DualSense Wireless Controller.")

    if classification.hid_devices:
        print("PASS: Windows sees a DualSense HID function.")
    if classification.audio_devices:
        print("PASS: Windows sees a DualSense USB Audio function.")

    if classification.ch340_devices:
        print("CH340 serial ports are present; these cannot become the DualSense USB composite device in firmware.")
        for device in classification.ch340_devices:
            print_device(device)

    if classification.descriptor_failures:
        print("USB descriptor failure device(s) are present; check D+/D-, cable length, and power.")
        for device in classification.descriptor_failures:
            print_device(device)

    if not classification.dualsense_devices:
        print("Next hardware check: connect BL618 USB_DP -> USB D+, USB_DM -> USB D-, and GND -> GND.")
        print("If the board was just flashed through UART download mode, press RST/EN once to boot the app.")


def self_test() -> int:
    samples = {
        "dualsense": json.dumps([
            {
                "Status": "OK",
                "Class": "USB",
                "FriendlyName": "USB Composite Device",
                "InstanceId": r"USB\VID_054C&PID_0CE6\M61DS5COMPOSITE1",
            },
            {
                "Status": "OK",
                "Class": "HIDClass",
                "FriendlyName": "HID-compliant game controller",
                "InstanceId": r"HID\VID_054C&PID_0CE6&MI_03\8&1",
            },
            {
                "Status": "OK",
                "Class": "MEDIA",
                "FriendlyName": "USB Audio Device",
                "InstanceId": r"USB\VID_054C&PID_0CE6&MI_00\8&2",
            }
        ]),
        "ch340": json.dumps([
            {
                "Status": "OK",
                "Class": "Ports",
                "FriendlyName": "USB-SERIAL CH340 (COM5)",
                "InstanceId": r"USB\VID_1A86&PID_7523\6&3B164EBC&0&2",
            }
        ]),
        "descriptor_fail": json.dumps([
            {
                "Status": "Unknown",
                "Class": "USB",
                "FriendlyName": "Unknown USB Device (Device Descriptor Request Failed)",
                "InstanceId": r"USB\VID_0000&PID_0002\6&3B164EBC&0&1",
            }
        ]),
    }

    checks = [
        ("dualsense detected", classify_devices(parse_devices(samples["dualsense"])).found_m61),
        ("dualsense hid detected", classify_devices(parse_devices(samples["dualsense"])).found_hid),
        ("dualsense audio detected", classify_devices(parse_devices(samples["dualsense"])).found_audio),
        ("ch340 not m61", not classify_devices(parse_devices(samples["ch340"])).found_m61),
        (
            "descriptor failure classified",
            bool(classify_devices(parse_devices(samples["descriptor_fail"])).descriptor_failures),
        ),
    ]
    failed = [name for name, ok in checks if not ok]
    if failed:
        print("M61 USB Windows checker self-test failed:")
        for name in failed:
            print(f"  - {name}")
        return 1

    print("M61 USB Windows checker self-test passed.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample-json", type=Path, help="read saved Get-PnpDevice JSON instead of querying Windows")
    parser.add_argument("--json", action="store_true", help="print raw matched devices as JSON")
    parser.add_argument("--self-test", action="store_true", help="run parser/classifier self-test")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    try:
        raw = args.sample_json.read_text(encoding="utf-8") if args.sample_json else pnp_query_json()
        devices = parse_devices(raw)
    except Exception as exc:
        print(f"USB PnP query failed: {exc}", file=sys.stderr)
        return 1

    classification = classify_devices(devices)
    if args.json:
        print(json.dumps([device.__dict__ for device in classification.matching_devices], indent=2))
    else:
        print_report(classification)

    return 0 if classification.found_m61 else 2


if __name__ == "__main__":
    raise SystemExit(main())
