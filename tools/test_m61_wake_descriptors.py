#!/usr/bin/env python3
"""Validate the M61 keyboard, BOS, and MS OS 2.0 descriptor contract."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USB_C = ROOT / "m61" / "dualsense_hidp_probe" / "m61_usb_gamepad.c"


def expected_bos(vendor_code: int = 0x01) -> bytes:
    return bytes.fromhex(
        "05 0f 21 00 01 1c 10 05 00 "
        "df 60 dd d8 89 45 c7 4c 9c d2 65 9d 9e 64 8a 9f "
        f"00 00 03 06 58 00 {vendor_code:02x} 00"
    )


def expected_ms_os_20() -> bytes:
    name = "SelectiveSuspendEnabled\0".encode("utf-16le")
    return (
        bytes.fromhex("0a 00 00 00 00 00 03 06 58 00")
        + bytes.fromhex("08 00 01 00 00 00 4e 00")
        + bytes.fromhex("08 00 02 00 00 00 46 00")
        + bytes.fromhex("3e 00 04 00 04 00 30 00")
        + name
        + bytes.fromhex("04 00 01 00 00 00")
    )


def main() -> int:
    bos = expected_bos()
    ms_os = expected_ms_os_20()
    assert len(bos) == 33
    assert bos[2:4] == (33).to_bytes(2, "little")
    assert bos[9:25] == bytes.fromhex("df60ddd88945c74c9cd2659d9e648a9f")
    assert bos[29:31] == (88).to_bytes(2, "little")
    assert bos[31] == 0x01

    assert len(ms_os) == 88
    assert ms_os[8:10] == (88).to_bytes(2, "little")
    assert ms_os[16:18] == (78).to_bytes(2, "little")
    assert ms_os[24:26] == (70).to_bytes(2, "little")
    assert ms_os[26:28] == (62).to_bytes(2, "little")
    assert ms_os[32:34] == (48).to_bytes(2, "little")
    assert ms_os[34:82].decode("utf-16le") == "SelectiveSuspendEnabled\0"
    assert ms_os[82:84] == (4).to_bytes(2, "little")
    assert ms_os[84:88] == bytes.fromhex("01000000")

    source = USB_C.read_text(encoding="utf-8")
    for snippet in (
        "USB_DUALSENSE_CONFIG_DESC_BASE_SIZE 0x00E3",
        "USB_DUALSENSE_CONFIG_DESC_KBD_SIZE 25",
        "ITF_NUM_HID_KBD 4",
        "HID_KBD_IN_EP 0x87",
        "HID_KBD_REPORT_DESC_SIZE 45",
        "USB_BOS_HEADER_DESCRIPTOR_INIT(BOS_DESC_LEN, 1)",
        "USB_BOS_CAP_PLATFORM_WINUSB_DESCRIPTOR_INIT(MS_OS_20_VENDOR_CODE",
        "USB_MSOSV2_COMP_ID_SET_HEADER_DESCRIPTOR_INIT(MS_OS_20_DESC_LEN)",
        "'S', 0, 'e', 0, 'l', 0, 'e', 0, 'c', 0, 't', 0",
        ".msosv2_descriptor = &wake_ms_os_20",
        ".bos_descriptor = &wake_bos",
        "descriptor_wake_enabled ? USB_2_1 : USB_2_0",
        "descriptor_keyboard_enabled = descriptor_wake_enabled ||",
        "if (!descriptor_wake_enabled || !usb_suspended)",
        "config_descriptor_runtime[4] = descriptor_keyboard_enabled ?",
        "event == USBD_EVENT_RESET &&",
        "if (usb_wake_state == USB_WAKE_REQUESTED)",
        "USB_WAKE_SETTLE_MS 150U",
        "USB_WAKE_KEY_HOLD_MS 80U",
        "USB_WAKE_KEY_UP_SETTLE_MS 200U",
        "USB_WAKE_KEY_ATTEMPTS 2U",
        "HID_KEY_F15 0x68U",
    ):
        assert snippet in source, f"missing wake descriptor contract: {snippet}"

    print("M61 wake descriptor tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
