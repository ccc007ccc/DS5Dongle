#!/usr/bin/env python3
"""Regression checks for the M61 USB-to-Bluetooth Feature Report bridge."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GAMEPAD_SOURCE = ROOT / "m61" / "dualsense_hidp_probe" / "m61_usb_gamepad.c"
MAIN_SOURCE = ROOT / "m61" / "dualsense_hidp_probe" / "main.c"


def bluetooth_feature_to_usb(packet: bytes) -> bytes:
    """Remove the HIDP transaction byte, retaining the HID Report ID."""
    assert len(packet) >= 3 and packet[0] == 0xA3
    return packet[1:]


class FeatureCacheModel:
    def __init__(self) -> None:
        self.reports: dict[int, bytes] = {}

    def store_bluetooth(self, packet: bytes) -> None:
        report = bluetooth_feature_to_usb(packet)
        self.reports[report[0]] = report

    def set_from_usb(self, report_id: int) -> None:
        if report_id == 0x80:
            self.reports.pop(0x81, None)

    def host_probe_ready(self) -> bool:
        required = {0x09: 20, 0x20: 64, 0x05: 41}
        return all(
            len(self.reports.get(report_id, b"")) >= length
            for report_id, length in required.items()
        )


def check_report_id_alignment() -> None:
    expected_mac = bytes.fromhex("7c66ef4c6297")
    bt_packet = bytes((0xA3, 0x09)) + expected_mac + bytes(13)
    usb_report = bluetooth_feature_to_usb(bt_packet)

    assert len(usb_report) == 20
    assert usb_report[0] == 0x09
    assert usb_report[1:7] == expected_mac


def check_dynamic_page_invalidation() -> None:
    cache = FeatureCacheModel()
    first_page = bytes((0xA3, 0x81, 0x10, 0x02, 0x00)) + bytes(60)
    second_page = bytes((0xA3, 0x81, 0x10, 0x03, 0x00)) + bytes(60)

    cache.store_bluetooth(first_page)
    assert cache.reports[0x81][2] == 0x02
    cache.set_from_usb(0x80)
    assert 0x81 not in cache.reports
    cache.store_bluetooth(second_page)
    assert cache.reports[0x81][2] == 0x03


def check_linux_host_probe_prefetch() -> None:
    cache = FeatureCacheModel()
    assert not cache.host_probe_ready()
    cache.store_bluetooth(bytes((0xA3, 0x09)) + bytes(19))
    cache.store_bluetooth(bytes((0xA3, 0x20)) + bytes(63))
    assert not cache.host_probe_ready()
    cache.store_bluetooth(bytes((0xA3, 0x05)) + bytes(40))
    assert cache.host_probe_ready()


def check_source_wiring() -> None:
    gamepad = GAMEPAD_SOURCE.read_text(encoding="utf-8")
    main = MAIN_SOURCE.read_text(encoding="utf-8")

    required_gamepad = (
        "#define FEATURE_CACHE_SLOTS 32",
        "#define FEATURE_GET_QUEUE_DEPTH 16",
        "invalidate_feature_cache_locked(0x81);",
        "usb_diag.feature_get_queue_coalesced++",
        "remove_queued_feature_request_locked(report_id);",
        "report_id >= M61_WEB_COMMAND_REPORT_ID",
        "report_id <= M61_WEB_TELEMETRY_REPORT_ID",
        "m61_web_config_encode(",
        "m61_web_telemetry_encode(",
        "host_probe_feature_requirements[]",
        "DS5_FEATURE_REPORT_PAIRING_INFO_SIZE 20U",
        "DS5_FEATURE_REPORT_FIRMWARE_INFO_SIZE 64U",
        "DS5_FEATURE_REPORT_CALIBRATION_SIZE 41U",
        "m61_audio_class_interface_request_handler(",
        "setup->bRequest == AUDIO_REQUEST_SET_RES",
        "m61_audio_init_intf(0,",
        "0x06,\n    AUDIO_INPUT_CHANNELS,",
        "0x01, 0x03,\n    0x04,\n    AUDIO_SPEAKER_FU_ID,",
        "AUDIO_OUT_PACKET_SIZE >> 8) & 0xFF,\n    0x01,",
        "AUDIO_IN_PACKET_SIZE >> 8) & 0xFF,\n    0x01,",
    )
    required_main = (
        "buf->data + 1,",
        "buf->len - 1);",
        "Factory/system queries use SET 0x80",
        "m61_usb_gamepad_request_feature_report(",
        "0x81, M61_DS5_USB_FEATURE_MAX_LEN",
        "handle_m61_web_command(&host_report)",
        "feature_id == M61_WEB_COMMAND_REPORT_ID",
        "M61_WEB_CONFIG_KEY",
        "load_m61_web_config()",
        "bt_check_if_ef_ready() == EF_NO_ERR",
        "save_m61_web_config()",
        "migrate_legacy_dvfs_record(unified_config_applied)",
        "err = save_m61_web_config();",
        "err = save_release_default_dvfs_policy();",
        "apply_m61_web_config(&config, true)",
        "m61_usb_gamepad_set_audio_speaker_enabled",
        "m61_usb_gamepad_set_haptics_gain_q8",
        "M61_WEB_COMMAND_POWER_OFF_CONTROLLER",
        "hidp_power_off_controller()",
        "M61_WEB_COMMAND_PAIR_CONTROLLER",
        "M61_WEB_COMMAND_DISCONNECT_CONTROLLER",
        "M61_WEB_COMMAND_FORGET_CONTROLLER",
        "bt_unpair(BT_ID_DEFAULT, NULL)",
        "m61_usb_gamepad_request_host_probe_features();",
        "m61_usb_gamepad_host_probe_features_ready()",
        "if (usb_start_after_dualsense_done &&",
    )
    for snippet in required_gamepad:
        assert snippet in gamepad, f"missing gamepad bridge logic: {snippet}"
    for snippet in required_main:
        assert snippet in main, f"missing HIDP bridge logic: {snippet}"
    assert "if (easyflash_init() == EF_NO_ERR)" not in main

    clock_command = main[main.index("static int m61_clock_command"):]
    assert "err = m61_dvfs_save_persistent_config();" not in clock_command

    set_position = main.index("Factory/system queries use SET 0x80")
    get_position = main.index(
        "while (feature_reports_this_tick < CONFIG_M61_DS5_FEATURE_REPORTS_PER_TICK)",
        set_position,
    )
    assert set_position < get_position, "Feature SET must be forwarded before Feature GET"


def main() -> None:
    check_report_id_alignment()
    check_dynamic_page_invalidation()
    check_linux_host_probe_prefetch()
    check_source_wiring()
    print("M61 Feature Report bridge regression checks: PASS")


if __name__ == "__main__":
    main()
