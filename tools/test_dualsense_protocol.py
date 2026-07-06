#!/usr/bin/env python3
"""Offline checks for the DualSense 0x31 report map used by stage 1.

This is not a replacement for compiling and running the ESP32 firmware. It
keeps the documented field offsets, parser expectations, and synthetic vectors
aligned while ESP-IDF/hardware are unavailable.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
PARSER = ROOT / "main" / "dualsense_parser.c"
OUTPUT = ROOT / "main" / "dualsense_output.c"
M61_USB = ROOT / "m61" / "dualsense_hidp_probe" / "m61_usb_gamepad.c"
M61_MAIN = ROOT / "m61" / "dualsense_hidp_probe" / "main.c"
M61_DSE = ROOT / "m61" / "dualsense_hidp_probe" / "m61_ds5_dse.c"
M61_CMAKE = ROOT / "m61" / "dualsense_hidp_probe" / "CMakeLists.txt"
M61_BUILD_SH = ROOT / "m61" / "dualsense_hidp_probe" / "build.sh"
M61_TRANSPORT = ROOT / "m61" / "dualsense_hidp_probe" / "m61_esp32_transport.c"
M61_LEFT_SPI_EXAMPLE = ROOT / "m61" / "dualsense_hidp_probe" / "defconfig.dual_chip_left_spi.example"
FIRMWARE_MANIFEST = ROOT / "tools" / "firmware_manifest.py"
CHECK_DUAL_CHIP_LOG = ROOT / "tools" / "check_dual_chip_log.py"
DUAL_PROTO = ROOT / "main" / "dual_chip_spi_proto.c"
ESP32_DUAL_SPI = ROOT / "main" / "esp32_dual_chip_spi.c"
ESP32_RAW_HIDP = ROOT / "main" / "bt_dualsense_raw_hidp.c"
ESP32_LED_STATUS = ROOT / "main" / "led_status.h"
ESP32_DUAL_DEFAULTS = ROOT / "sdkconfig.dual_chip.defaults"
ESP32_DUAL_LEFT_DEFAULTS = ROOT / "sdkconfig.dual_chip.devkit_left.defaults"
ESP32_DUAL_VSPI_DEFAULTS = ROOT / "sdkconfig.dual_chip.devkit_vspi.defaults"
DUAL_WIRING_DOC = ROOT / "docs" / "DUAL_CHIP_WIRING.md"

DS5_BT_HIDP_INPUT = 0xA1
DS5_BT_INPUT_REPORT_ID = 0x31
DS5_MIN_PAYLOAD_LEN = 55
DS5_BASIC_INPUT_REPORT_ID = 0x01
DS5_BASIC_PAYLOAD_LEN = 9
DS5_OUTPUT_REPORT31_BT_LEN = 78
DS5_OUTPUT_REPORT32_BT_LEN = 142
DS5_OUTPUT_REPORT36_BT_LEN = 398
DS5_OUTPUT_SET_STATE_LEN = 63
DS5_OUTPUT_HAPTICS_BLOCK_LEN = 64
DS5_OUTPUT_SPEAKER_OPUS_LEN = 200
DS5_DUAL_SPI_MAGIC = 0x3544
DS5_DUAL_SPI_VERSION = 1
DS5_DUAL_SPI_HEADER_LEN = 20
DS5_DUAL_SPI_MAX_PAYLOAD = 512
DS5_DUAL_HELLO_PAYLOAD_LEN = 16
DS5_DUAL_TIME_SYNC_PAYLOAD_LEN = 16
DS5_DUAL_BT_STATE_PAYLOAD_LEN = 24
DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN = 16
DS5_DUAL_ACK_PAYLOAD_LEN = 8
DS5_DUAL_STATS_PAYLOAD_LEN = 84
DS5_DUAL_MSG_HELLO = 1
DS5_DUAL_MSG_TIME_SYNC = 2
DS5_DUAL_MSG_BT_CONNECT = 3
DS5_DUAL_MSG_BT_DISCONNECT = 4
DS5_DUAL_MSG_BT_STATE = 5
DS5_DUAL_MSG_BT_TX_AUDIO_RT = 9
DS5_DUAL_MSG_FLOW_CREDIT = 10
DS5_DUAL_MSG_STATS = 11
DS5_DUAL_MSG_RESET_STATS = 12
DS5_DUAL_MSG_WIRE_TEST = 16
DS5_DUAL_MSG_BT_FORGET = 17
DS5_DUAL_FORGET_SAVED_ADDR = 0x01
DS5_DUAL_FORGET_BONDS = 0x02
DS5_DUAL_CHANNEL_AUDIO = 4
DS5_DUAL_CHANNEL_STATUS = 5
DS5_DUAL_CHANNEL_CTRL = 1
DS5_DUAL_PRIORITY_RT = 1
DS5_DUAL_PRIORITY_CONTROL = 4
DS5_DUAL_PRIORITY_LOW = 5
DS5_DUAL_FLAG_RELIABLE = 0x0001
DS5_DUAL_FLAG_LATEST = 0x0002
DS5_DUAL_FLAG_DROP_OK = 0x0004
DS5_DUAL_FLAG_ACK = 0x0008
DS5_DUAL_ROLE_M61_USB = 1
DS5_DUAL_CAP_USB_DS5_GADGET = 0x00000001
DS5_DUAL_CAP_AUDIO_RT = 0x00000004
DS5_DUAL_CAP_FEATURE_REPORTS = 0x00000008
DS5_DUAL_CAP_FLOW_CREDIT = 0x00000010
DS5_DUAL_CAP_RELIABLE_ACK = 0x00000020
DS5_DUAL_BT_STATE_READY = 0x00000001
DS5_DUAL_BT_STATE_L2CAP_READY = 0x00000002
DS5_DUAL_BT_STATE_SDP_READY = 0x00000004
DS5_DUAL_BT_STATE_CONTROL_OPEN = 0x00000008
DS5_DUAL_BT_STATE_INTERRUPT_OPEN = 0x00000010
DS5_DUAL_BT_STATE_FULL_REPORT = 0x00000020

BUTTON_SQUARE = 1 << 0
BUTTON_CROSS = 1 << 1
BUTTON_CIRCLE = 1 << 2
BUTTON_TRIANGLE = 1 << 3
BUTTON_L1 = 1 << 4
BUTTON_R1 = 1 << 5
BUTTON_L2 = 1 << 6
BUTTON_R2 = 1 << 7
BUTTON_CREATE = 1 << 8
BUTTON_OPTIONS = 1 << 9
BUTTON_L3 = 1 << 10
BUTTON_R3 = 1 << 11
BUTTON_PS = 1 << 12
BUTTON_TOUCHPAD = 1 << 13
BUTTON_MUTE = 1 << 14
BUTTON_EDGE_FN_L = 1 << 15
BUTTON_EDGE_FN_R = 1 << 16
BUTTON_EDGE_PADDLE_L = 1 << 17
BUTTON_EDGE_PADDLE_R = 1 << 18

DEFAULT_SET_STATE = bytes(
    [
        0xFD, 0xF7, 0x00, 0x00, 0x64, 0x64, 0xFF, 0x09,
        0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00,
        0x00, 0x02, 0x01, 0x00, 0xFF, 0xD7, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ]
)


@dataclass(frozen=True)
class Parsed:
    report_id: int
    sequence: int
    payload_offset: int
    is_full_report: bool
    has_motion: bool
    has_battery: bool
    left_x: int
    left_y: int
    right_x: int
    right_y: int
    l2: int
    r2: int
    dpad: int
    buttons: int
    gyro_x: int
    gyro_y: int
    gyro_z: int
    accel_x: int
    accel_y: int
    accel_z: int
    sensor_timestamp: int
    temperature: int
    battery_percent: int
    power_state: int
    headphones: bool
    mic_present: bool
    mic_muted: bool
    usb_data: bool
    usb_power: bool


def read_i16_le(data: bytes, offset: int) -> int:
    value = data[offset] | (data[offset + 1] << 8)
    return value - 0x10000 if value & 0x8000 else value


def read_u32_le(data: bytes, offset: int) -> int:
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def put_i16_le(data: bytearray, offset: int, value: int) -> None:
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF


def put_u32_le(data: bytearray, offset: int, value: int) -> None:
    data[offset] = value & 0xFF
    data[offset + 1] = (value >> 8) & 0xFF
    data[offset + 2] = (value >> 16) & 0xFF
    data[offset + 3] = (value >> 24) & 0xFF


def crc32_le_update(crc: int, data: bytes) -> int:
    for value in data:
        crc ^= value
        for _ in range(8):
            mask = 0xFFFFFFFF if crc & 1 else 0
            crc = ((crc >> 1) ^ (0xEDB88320 & mask)) & 0xFFFFFFFF
    return crc


def dualsense_output_crc32(data: bytes) -> int:
    crc = crc32_le_update(0xFFFFFFFF, bytes([0xA2]))
    crc = crc32_le_update(crc, data)
    return (~crc) & 0xFFFFFFFF


def dual_spi_crc32(header_without_crc: bytes, payload: bytes) -> int:
    crc = crc32_le_update(0xFFFFFFFF, header_without_crc)
    crc = crc32_le_update(crc, payload)
    return (~crc) & 0xFFFFFFFF


def make_dual_spi_frame(
    msg_type: int,
    flags: int,
    channel: int,
    priority: int,
    seq: int,
    deadline: int,
    payload: bytes,
) -> bytes:
    assert len(payload) <= DS5_DUAL_SPI_MAX_PAYLOAD
    header = bytearray(DS5_DUAL_SPI_HEADER_LEN)
    header[0:2] = DS5_DUAL_SPI_MAGIC.to_bytes(2, "little")
    header[2] = DS5_DUAL_SPI_VERSION
    header[3] = msg_type
    header[4:6] = flags.to_bytes(2, "little")
    header[6] = channel
    header[7] = priority
    header[8:10] = seq.to_bytes(2, "little")
    header[10:14] = deadline.to_bytes(4, "little")
    header[14:16] = len(payload).to_bytes(2, "little")
    header[16:20] = dual_spi_crc32(bytes(header[:16]), payload).to_bytes(4, "little")
    return bytes(header) + payload


def decode_dual_flow_credit(payload: bytes) -> tuple[int, int, bool, int, int, int]:
    assert len(payload) >= DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN
    last_err_raw = int.from_bytes(payload[12:16], "little")
    last_err = last_err_raw - 0x100000000 if last_err_raw & 0x80000000 else last_err_raw
    return (
        payload[0],
        payload[1],
        payload[2] != 0,
        int.from_bytes(payload[4:8], "little"),
        int.from_bytes(payload[8:12], "little"),
        last_err,
    )


def make_dual_ack(seq: int, msg_type: int, status: int) -> bytes:
    payload = bytearray(DS5_DUAL_ACK_PAYLOAD_LEN)
    payload[0:2] = seq.to_bytes(2, "little")
    payload[2] = msg_type
    payload[4:8] = (status & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(payload)


def decode_dual_ack(payload: bytes) -> tuple[int, int, int]:
    assert len(payload) >= DS5_DUAL_ACK_PAYLOAD_LEN
    status_raw = int.from_bytes(payload[4:8], "little")
    status = status_raw - 0x100000000 if status_raw & 0x80000000 else status_raw
    return int.from_bytes(payload[0:2], "little"), payload[2], status


def make_dual_hello(role: int, queue_depth: int, capabilities: int) -> bytes:
    payload = bytearray(DS5_DUAL_HELLO_PAYLOAD_LEN)
    payload[0] = DS5_DUAL_SPI_VERSION
    payload[1] = role
    payload[2:4] = DS5_DUAL_SPI_HEADER_LEN.to_bytes(2, "little")
    payload[4:6] = DS5_DUAL_SPI_MAX_PAYLOAD.to_bytes(2, "little")
    payload[6:8] = (DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD).to_bytes(2, "little")
    payload[8] = queue_depth
    payload[10:14] = capabilities.to_bytes(4, "little")
    return bytes(payload)


def decode_dual_hello(payload: bytes) -> tuple[int, int, int, int, int, int, int]:
    assert len(payload) >= DS5_DUAL_HELLO_PAYLOAD_LEN
    return (
        payload[0],
        payload[1],
        int.from_bytes(payload[2:4], "little"),
        int.from_bytes(payload[4:6], "little"),
        int.from_bytes(payload[6:8], "little"),
        payload[8],
        int.from_bytes(payload[10:14], "little"),
    )


def make_dual_time_sync(m61_time_us: int, esp_time_us: int, rtt_us: int, offset_us: int) -> bytes:
    payload = bytearray(DS5_DUAL_TIME_SYNC_PAYLOAD_LEN)
    payload[0:4] = (m61_time_us & 0xFFFFFFFF).to_bytes(4, "little")
    payload[4:8] = (esp_time_us & 0xFFFFFFFF).to_bytes(4, "little")
    payload[8:12] = (rtt_us & 0xFFFFFFFF).to_bytes(4, "little")
    payload[12:16] = (offset_us & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(payload)


def decode_dual_time_sync(payload: bytes) -> tuple[int, int, int, int]:
    assert len(payload) >= DS5_DUAL_TIME_SYNC_PAYLOAD_LEN
    offset_raw = int.from_bytes(payload[12:16], "little")
    offset_us = offset_raw - 0x100000000 if offset_raw & 0x80000000 else offset_raw
    return (
        int.from_bytes(payload[0:4], "little"),
        int.from_bytes(payload[4:8], "little"),
        int.from_bytes(payload[8:12], "little"),
        offset_us,
    )


def make_dual_bt_state(flags: int, last_error: int, rssi: int, state_seq: int) -> bytes:
    payload = bytearray(DS5_DUAL_BT_STATE_PAYLOAD_LEN)
    payload[0:4] = flags.to_bytes(4, "little")
    payload[4:8] = (last_error & 0xFFFFFFFF).to_bytes(4, "little")
    payload[8] = rssi & 0xFF
    payload[9] = 7
    payload[10] = 2
    payload[12:14] = (672).to_bytes(2, "little")
    payload[14:16] = (672).to_bytes(2, "little")
    payload[16:22] = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])
    payload[22:24] = state_seq.to_bytes(2, "little")
    return bytes(payload)


def decode_dual_bt_state(payload: bytes) -> tuple[int, int, int, int, int, int, int, bytes, int]:
    assert len(payload) >= DS5_DUAL_BT_STATE_PAYLOAD_LEN
    err_raw = int.from_bytes(payload[4:8], "little")
    last_error = err_raw - 0x100000000 if err_raw & 0x80000000 else err_raw
    rssi = payload[8] - 0x100 if payload[8] & 0x80 else payload[8]
    return (
        int.from_bytes(payload[0:4], "little"),
        last_error,
        rssi,
        payload[9],
        payload[10],
        int.from_bytes(payload[12:14], "little"),
        int.from_bytes(payload[14:16], "little"),
        payload[16:22],
        int.from_bytes(payload[22:24], "little"),
    )


def make_dual_stats(role: int, values: list[int]) -> bytes:
    assert len(values) == 20
    payload = bytearray(DS5_DUAL_STATS_PAYLOAD_LEN)
    payload[0] = DS5_DUAL_SPI_VERSION
    payload[1] = role
    for index, value in enumerate(values):
        offset = 4 + index * 4
        payload[offset:offset + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(payload)


def decode_dual_stats(payload: bytes) -> tuple[int, int, tuple[int, ...]]:
    assert len(payload) >= DS5_DUAL_STATS_PAYLOAD_LEN
    values = tuple(
        int.from_bytes(payload[4 + index * 4:8 + index * 4], "little")
        for index in range(20)
    )
    return payload[0], payload[1], values


def fill_output_crc(report: bytearray) -> None:
    crc = dualsense_output_crc32(bytes(report[:-4]))
    report[-4] = crc & 0xFF
    report[-3] = (crc >> 8) & 0xFF
    report[-2] = (crc >> 16) & 0xFF
    report[-1] = (crc >> 24) & 0xFF


def make_output31(seq: int) -> bytes:
    report = bytearray(DS5_OUTPUT_REPORT31_BT_LEN)
    report[0] = 0x31
    report[1] = (seq & 0x0F) << 4
    report[2] = 0x10
    report[3:3 + DS5_OUTPUT_SET_STATE_LEN] = DEFAULT_SET_STATE
    fill_output_crc(report)
    return bytes(report)


def make_output32(seq: int) -> bytes:
    report = bytearray(DS5_OUTPUT_REPORT32_BT_LEN)
    report[0] = 0x32
    report[1] = (seq & 0x0F) << 4
    report[2] = 0x90
    report[3] = DS5_OUTPUT_SET_STATE_LEN
    report[4:4 + DS5_OUTPUT_SET_STATE_LEN] = DEFAULT_SET_STATE
    fill_output_crc(report)
    return bytes(report)


def make_output32_audio_status(seq: int, packet_counter: int, mic_active: bool) -> bytes:
    report = bytearray(DS5_OUTPUT_REPORT32_BT_LEN)
    report[0] = 0x32
    report[1] = (seq & 0x0F) << 4
    report[2] = 0x91
    report[3] = 7
    report[4] = 0xFF if mic_active else 0xFE
    report[5:10] = bytes([64, 64, 64, 64, 64])
    report[10] = packet_counter & 0xFF
    fill_output_crc(report)
    return bytes(report)


def make_output36_haptics(seq: int, packet_counter: int, haptics: bytes, mic_active: bool = False) -> bytes:
    report = bytearray(DS5_OUTPUT_REPORT36_BT_LEN)
    report[0] = 0x36
    report[1] = (seq & 0x0F) << 4
    report[2] = 0x91
    report[3] = 7
    report[4] = 0xFF if mic_active else 0xFE
    report[5:10] = bytes([64, 64, 64, 64, 64])
    report[10] = packet_counter & 0xFF
    report[11] = 0x90
    report[12] = DS5_OUTPUT_SET_STATE_LEN
    report[13:13 + DS5_OUTPUT_SET_STATE_LEN] = DEFAULT_SET_STATE
    report[76] = 0x92
    report[77] = DS5_OUTPUT_HAPTICS_BLOCK_LEN
    report[78:78 + DS5_OUTPUT_HAPTICS_BLOCK_LEN] = haptics
    fill_output_crc(report)
    return bytes(report)


def make_output36_audio(seq: int, packet_counter: int, haptics: bytes, opus: bytes, headset: bool) -> bytes:
    report = bytearray(make_output36_haptics(seq, packet_counter, haptics, mic_active=True))
    report[142] = 0x96 if headset else 0x93
    report[143] = DS5_OUTPUT_SPEAKER_OPUS_LEN
    report[144:144 + DS5_OUTPUT_SPEAKER_OPUS_LEN] = opus
    fill_output_crc(report)
    return bytes(report)


def make_payload() -> bytes:
    payload = bytearray(63)
    payload[0] = 12
    payload[1] = 34
    payload[2] = 56
    payload[3] = 78
    payload[4] = 90
    payload[5] = 123
    payload[7] = 0x70 | 3  # square + cross + circle + dpad SE
    payload[8] = 0xA5      # L1 + L2 + Options + R3
    payload[9] = 0x57      # PS + touchpad + mute + FnL + PaddleL
    put_i16_le(payload, 15, -1000)
    put_i16_le(payload, 17, 2000)
    put_i16_le(payload, 19, -3000)
    put_i16_le(payload, 21, 4000)
    put_i16_le(payload, 23, -5000)
    put_i16_le(payload, 25, 6000)
    put_u32_le(payload, 27, 0x12345678)
    payload[31] = 0xF6  # -10
    payload[52] = 0x25  # power=2, battery=5
    payload[53] = 0x1F
    return bytes(payload)


def parse_reference(data: bytes) -> Parsed | str:
    report_id = DS5_BT_INPUT_REPORT_ID
    sequence = 0
    payload_offset = 0
    is_full_report = False

    if len(data) >= 3 and data[0] == DS5_BT_HIDP_INPUT and data[1] == DS5_BT_INPUT_REPORT_ID:
        report_id = data[1]
        sequence = data[2]
        payload_offset = 3
        is_full_report = True
        if data[2] & 0x02:
            return "mic"
    elif len(data) >= 2 and data[0] == DS5_BT_INPUT_REPORT_ID:
        report_id = data[0]
        sequence = data[1]
        payload_offset = 2
        is_full_report = True
        if data[1] & 0x02:
            return "mic"
    elif (
        len(data) >= 2 + DS5_BASIC_PAYLOAD_LEN
        and data[0] == DS5_BT_HIDP_INPUT
        and data[1] == DS5_BASIC_INPUT_REPORT_ID
    ):
        report_id = data[1]
        payload_offset = 2
    elif len(data) >= 1 + DS5_BASIC_PAYLOAD_LEN and data[0] == DS5_BASIC_INPUT_REPORT_ID:
        report_id = data[0]
        payload_offset = 1
    elif len(data) >= DS5_MIN_PAYLOAD_LEN:
        payload_offset = 0
        is_full_report = True
    else:
        return "unknown"

    payload = data[payload_offset:]
    if (is_full_report and len(payload) < DS5_MIN_PAYLOAD_LEN) or (
        not is_full_report and len(payload) < DS5_BASIC_PAYLOAD_LEN
    ):
        return "unknown"

    buttons = 0
    for value, bit, mask in [
        (payload[7], 0x10, BUTTON_SQUARE),
        (payload[7], 0x20, BUTTON_CROSS),
        (payload[7], 0x40, BUTTON_CIRCLE),
        (payload[7], 0x80, BUTTON_TRIANGLE),
        (payload[8], 0x01, BUTTON_L1),
        (payload[8], 0x02, BUTTON_R1),
        (payload[8], 0x04, BUTTON_L2),
        (payload[8], 0x08, BUTTON_R2),
        (payload[8], 0x10, BUTTON_CREATE),
        (payload[8], 0x20, BUTTON_OPTIONS),
        (payload[8], 0x40, BUTTON_L3),
        (payload[8], 0x80, BUTTON_R3),
    ]:
        if value & bit:
            buttons |= mask
    if len(payload) > 9:
        for value, bit, mask in [
            (payload[9], 0x01, BUTTON_PS),
            (payload[9], 0x02, BUTTON_TOUCHPAD),
            (payload[9], 0x04, BUTTON_MUTE),
            (payload[9], 0x10, BUTTON_EDGE_FN_L),
            (payload[9], 0x20, BUTTON_EDGE_FN_R),
            (payload[9], 0x40, BUTTON_EDGE_PADDLE_L),
            (payload[9], 0x80, BUTTON_EDGE_PADDLE_R),
        ]:
            if value & bit:
                buttons |= mask

    return Parsed(
        report_id=report_id,
        sequence=sequence,
        payload_offset=payload_offset,
        is_full_report=is_full_report,
        has_motion=is_full_report,
        has_battery=is_full_report,
        left_x=payload[0],
        left_y=payload[1],
        right_x=payload[2],
        right_y=payload[3],
        l2=payload[4],
        r2=payload[5],
        dpad=payload[7] & 0x0F,
        buttons=buttons,
        gyro_x=read_i16_le(payload, 15) if is_full_report else 0,
        gyro_z=read_i16_le(payload, 17) if is_full_report else 0,
        gyro_y=read_i16_le(payload, 19) if is_full_report else 0,
        accel_x=read_i16_le(payload, 21) if is_full_report else 0,
        accel_y=read_i16_le(payload, 23) if is_full_report else 0,
        accel_z=read_i16_le(payload, 25) if is_full_report else 0,
        sensor_timestamp=read_u32_le(payload, 27) if is_full_report else 0,
        temperature=read_i16_le(bytes([payload[31], 0xFF if payload[31] & 0x80 else 0]), 0)
        if is_full_report
        else 0,
        battery_percent=payload[52] & 0x0F if is_full_report else 0,
        power_state=payload[52] >> 4 if is_full_report else 0,
        headphones=bool(payload[53] & 0x01) if is_full_report else False,
        mic_present=bool(payload[53] & 0x02) if is_full_report else False,
        mic_muted=bool(payload[53] & 0x04) if is_full_report else False,
        usb_data=bool(payload[53] & 0x08) if is_full_report else False,
        usb_power=bool(payload[53] & 0x10) if is_full_report else False,
    )


def assert_vector(parsed: Parsed, offset: int, sequence: int) -> None:
    expected_buttons = (
        BUTTON_SQUARE
        | BUTTON_CROSS
        | BUTTON_CIRCLE
        | BUTTON_L1
        | BUTTON_L2
        | BUTTON_OPTIONS
        | BUTTON_R3
        | BUTTON_PS
        | BUTTON_TOUCHPAD
        | BUTTON_MUTE
        | BUTTON_EDGE_FN_L
        | BUTTON_EDGE_PADDLE_L
    )
    assert parsed.report_id == DS5_BT_INPUT_REPORT_ID
    assert parsed.sequence == sequence
    assert parsed.payload_offset == offset
    assert parsed.is_full_report
    assert parsed.has_motion
    assert parsed.has_battery
    assert parsed.left_x == 12
    assert parsed.left_y == 34
    assert parsed.right_x == 56
    assert parsed.right_y == 78
    assert parsed.l2 == 90
    assert parsed.r2 == 123
    assert parsed.dpad == 3
    assert parsed.buttons == expected_buttons
    assert parsed.gyro_x == -1000
    assert parsed.gyro_y == -3000
    assert parsed.gyro_z == 2000
    assert parsed.accel_x == 4000
    assert parsed.accel_y == -5000
    assert parsed.accel_z == 6000
    assert parsed.sensor_timestamp == 0x12345678
    assert parsed.temperature == -10
    assert parsed.battery_percent == 5
    assert parsed.power_state == 2
    assert parsed.headphones
    assert parsed.mic_present
    assert parsed.mic_muted
    assert parsed.usb_data
    assert parsed.usb_power


def test_vectors() -> None:
    payload = make_payload()
    assert_vector(parse_reference(bytes([0xA1, 0x31, 0x01]) + payload), 3, 1)
    assert_vector(parse_reference(bytes([0x31, 0x01]) + payload), 2, 1)
    assert_vector(parse_reference(payload), 0, 0)
    assert parse_reference(bytes([0xA1, 0x31, 0x03]) + payload) == "mic"
    assert parse_reference(bytes([0x31, 0x03]) + payload) == "mic"
    assert parse_reference(b"\xA1\x30") == "unknown"

    basic = parse_reference(bytes([0x01]) + payload[:DS5_BASIC_PAYLOAD_LEN])
    assert isinstance(basic, Parsed)
    assert basic.report_id == DS5_BASIC_INPUT_REPORT_ID
    assert basic.payload_offset == 1
    assert not basic.is_full_report
    assert not basic.has_motion
    assert not basic.has_battery
    assert basic.left_x == 12
    assert basic.r2 == 123
    assert basic.dpad == 3
    assert basic.buttons & BUTTON_SQUARE
    assert basic.buttons & BUTTON_R3
    assert not (basic.buttons & BUTTON_PS)

    hidp_basic = parse_reference(bytes([0xA1, 0x01]) + payload[:DS5_BASIC_PAYLOAD_LEN])
    assert isinstance(hidp_basic, Parsed)
    assert hidp_basic.report_id == DS5_BASIC_INPUT_REPORT_ID
    assert hidp_basic.payload_offset == 2
    assert not hidp_basic.is_full_report
    assert hidp_basic.buttons == basic.buttons


def test_output_vectors() -> None:
    assert len(DEFAULT_SET_STATE) == DS5_OUTPUT_SET_STATE_LEN

    report31 = make_output31(0)
    assert len(report31) == DS5_OUTPUT_REPORT31_BT_LEN
    assert report31[0:4] == bytes([0x31, 0x00, 0x10, 0xFD])
    assert report31[3:3 + DS5_OUTPUT_SET_STATE_LEN] == DEFAULT_SET_STATE
    assert int.from_bytes(report31[-4:], "little") == dualsense_output_crc32(report31[:-4])

    report32 = make_output32(1)
    assert len(report32) == DS5_OUTPUT_REPORT32_BT_LEN
    assert report32[0:5] == bytes([0x32, 0x10, 0x90, DS5_OUTPUT_SET_STATE_LEN, 0xFD])
    assert report32[4:4 + DS5_OUTPUT_SET_STATE_LEN] == DEFAULT_SET_STATE
    assert int.from_bytes(report32[-4:], "little") == dualsense_output_crc32(report32[:-4])

    mic_status = make_output32_audio_status(2, 9, mic_active=True)
    assert mic_status[0:11] == bytes([0x32, 0x20, 0x91, 7, 0xFF, 64, 64, 64, 64, 64, 9])
    assert int.from_bytes(mic_status[-4:], "little") == dualsense_output_crc32(mic_status[:-4])

    haptics = bytes(range(DS5_OUTPUT_HAPTICS_BLOCK_LEN))
    report36 = make_output36_haptics(3, 3, haptics)
    assert len(report36) == DS5_OUTPUT_REPORT36_BT_LEN
    assert report36[0:13] == bytes([0x36, 0x30, 0x91, 7, 0xFE, 64, 64, 64, 64, 64, 3, 0x90, 63])
    assert report36[13:13 + DS5_OUTPUT_SET_STATE_LEN] == DEFAULT_SET_STATE
    assert report36[76:78] == bytes([0x92, DS5_OUTPUT_HAPTICS_BLOCK_LEN])
    assert report36[78:78 + DS5_OUTPUT_HAPTICS_BLOCK_LEN] == haptics
    assert int.from_bytes(report36[-4:], "little") == dualsense_output_crc32(report36[:-4])

    opus = bytes([0x55] * DS5_OUTPUT_SPEAKER_OPUS_LEN)
    speaker_report = make_output36_audio(4, 10, haptics, opus, headset=False)
    headset_report = make_output36_audio(5, 11, haptics, opus, headset=True)
    assert speaker_report[142:144] == bytes([0x93, DS5_OUTPUT_SPEAKER_OPUS_LEN])
    assert speaker_report[144:144 + DS5_OUTPUT_SPEAKER_OPUS_LEN] == opus
    assert headset_report[142:144] == bytes([0x96, DS5_OUTPUT_SPEAKER_OPUS_LEN])
    assert int.from_bytes(speaker_report[-4:], "little") == dualsense_output_crc32(speaker_report[:-4])
    assert int.from_bytes(headset_report[-4:], "little") == dualsense_output_crc32(headset_report[:-4])


def test_dual_chip_spi_vectors() -> None:
    hello_caps = (
        DS5_DUAL_CAP_USB_DS5_GADGET
        | DS5_DUAL_CAP_AUDIO_RT
        | DS5_DUAL_CAP_FEATURE_REPORTS
        | DS5_DUAL_CAP_FLOW_CREDIT
        | DS5_DUAL_CAP_RELIABLE_ACK
    )
    hello_payload = make_dual_hello(DS5_DUAL_ROLE_M61_USB, 1, hello_caps)
    hello_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_HELLO,
        DS5_DUAL_FLAG_RELIABLE,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        1,
        0,
        hello_payload,
    )
    assert int.from_bytes(hello_frame[14:16], "little") == DS5_DUAL_HELLO_PAYLOAD_LEN
    assert decode_dual_hello(hello_frame[20:]) == (
        DS5_DUAL_SPI_VERSION,
        DS5_DUAL_ROLE_M61_USB,
        DS5_DUAL_SPI_HEADER_LEN,
        DS5_DUAL_SPI_MAX_PAYLOAD,
        DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD,
        1,
        hello_caps,
    )

    time_sync_payload = make_dual_time_sync(0x01020304, 0x11223344, 2500, -37)
    time_sync_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_TIME_SYNC,
        0,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        2,
        0,
        time_sync_payload,
    )
    assert int.from_bytes(time_sync_frame[14:16], "little") == DS5_DUAL_TIME_SYNC_PAYLOAD_LEN
    assert decode_dual_time_sync(time_sync_frame[20:]) == (0x01020304, 0x11223344, 2500, -37)

    bt_state_flags = (
        DS5_DUAL_BT_STATE_READY
        | DS5_DUAL_BT_STATE_L2CAP_READY
        | DS5_DUAL_BT_STATE_SDP_READY
        | DS5_DUAL_BT_STATE_CONTROL_OPEN
        | DS5_DUAL_BT_STATE_INTERRUPT_OPEN
        | DS5_DUAL_BT_STATE_FULL_REPORT
    )
    bt_state_payload = make_dual_bt_state(bt_state_flags, -110, -42, 0x1234)
    bt_state_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_BT_STATE,
        DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
        DS5_DUAL_CHANNEL_STATUS,
        DS5_DUAL_PRIORITY_CONTROL,
        3,
        0x1020,
        bt_state_payload,
    )
    assert int.from_bytes(bt_state_frame[14:16], "little") == DS5_DUAL_BT_STATE_PAYLOAD_LEN
    assert decode_dual_bt_state(bt_state_frame[20:]) == (
        bt_state_flags,
        -110,
        -42,
        7,
        2,
        672,
        672,
        bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]),
        0x1234,
    )

    payload = make_output36_audio(6, 12, bytes(range(DS5_OUTPUT_HAPTICS_BLOCK_LEN)), bytes([0x33] * 200), False)
    frame = make_dual_spi_frame(
        DS5_DUAL_MSG_BT_TX_AUDIO_RT,
        DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
        DS5_DUAL_CHANNEL_AUDIO,
        DS5_DUAL_PRIORITY_RT,
        0x1234,
        0,
        payload,
    )

    assert len(frame) == DS5_DUAL_SPI_HEADER_LEN + DS5_OUTPUT_REPORT36_BT_LEN
    assert frame[0:4] == bytes([0x44, 0x35, DS5_DUAL_SPI_VERSION, DS5_DUAL_MSG_BT_TX_AUDIO_RT])
    assert int.from_bytes(frame[4:6], "little") == DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK
    assert frame[6] == DS5_DUAL_CHANNEL_AUDIO
    assert frame[7] == DS5_DUAL_PRIORITY_RT
    assert int.from_bytes(frame[8:10], "little") == 0x1234
    assert int.from_bytes(frame[10:14], "little") == 0
    assert int.from_bytes(frame[14:16], "little") == len(payload)
    assert int.from_bytes(frame[16:20], "little") == dual_spi_crc32(frame[:16], payload)

    corrupted = bytearray(frame)
    corrupted[-1] ^= 0x80
    assert int.from_bytes(corrupted[16:20], "little") != dual_spi_crc32(corrupted[:16], corrupted[20:])

    credit_payload = bytearray(DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN)
    credit_payload[0] = 3
    credit_payload[1] = 4
    credit_payload[2] = 1
    credit_payload[4:8] = (7).to_bytes(4, "little")
    credit_payload[8:12] = (11).to_bytes(4, "little")
    credit_payload[12:16] = ((-22) & 0xFFFFFFFF).to_bytes(4, "little")
    credit_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_FLOW_CREDIT,
        DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
        DS5_DUAL_CHANNEL_STATUS,
        DS5_DUAL_PRIORITY_CONTROL,
        9,
        123456,
        bytes(credit_payload),
    )
    assert int.from_bytes(credit_frame[14:16], "little") == DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN
    assert decode_dual_flow_credit(credit_frame[20:]) == (3, 4, True, 7, 11, -22)

    stats_values = [0x1000 + index for index in range(20)]
    stats_payload = make_dual_stats(2, stats_values)
    stats_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_STATS,
        DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
        DS5_DUAL_CHANNEL_STATUS,
        DS5_DUAL_PRIORITY_LOW,
        10,
        654321,
        stats_payload,
    )
    assert int.from_bytes(stats_frame[14:16], "little") == DS5_DUAL_STATS_PAYLOAD_LEN
    assert int.from_bytes(stats_frame[16:20], "little") == dual_spi_crc32(stats_frame[:16], stats_payload)
    assert decode_dual_stats(stats_frame[20:]) == (
        DS5_DUAL_SPI_VERSION,
        2,
        tuple(stats_values),
    )

    ack_payload = make_dual_ack(0x3344, DS5_DUAL_MSG_RESET_STATS, 0)
    ack_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_RESET_STATS,
        DS5_DUAL_FLAG_ACK,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        10,
        0,
        ack_payload,
    )
    assert int.from_bytes(ack_frame[4:6], "little") == DS5_DUAL_FLAG_ACK
    assert int.from_bytes(ack_frame[14:16], "little") == DS5_DUAL_ACK_PAYLOAD_LEN
    assert decode_dual_ack(ack_frame[20:]) == (0x3344, DS5_DUAL_MSG_RESET_STATS, 0)

    reset_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_RESET_STATS,
        DS5_DUAL_FLAG_RELIABLE,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        0x3344,
        0,
        b"",
    )
    assert len(reset_frame) == DS5_DUAL_SPI_HEADER_LEN
    assert int.from_bytes(reset_frame[14:16], "little") == 0
    assert int.from_bytes(reset_frame[16:20], "little") == dual_spi_crc32(reset_frame[:16], b"")

    connect_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_BT_CONNECT,
        DS5_DUAL_FLAG_RELIABLE,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        0x3345,
        0,
        bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]),
    )
    assert int.from_bytes(connect_frame[14:16], "little") == 6
    assert connect_frame[20:26] == bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])

    disconnect_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_BT_DISCONNECT,
        DS5_DUAL_FLAG_RELIABLE,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        0x3346,
        0,
        b"\x00",
    )
    assert int.from_bytes(disconnect_frame[14:16], "little") == 1
    assert disconnect_frame[20] == 0

    wire_test_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_WIRE_TEST,
        DS5_DUAL_FLAG_RELIABLE,
        DS5_DUAL_CHANNEL_STATUS,
        DS5_DUAL_PRIORITY_CONTROL,
        0x3347,
        0,
        b"\x02",
    )
    assert int.from_bytes(wire_test_frame[14:16], "little") == 1
    assert wire_test_frame[20] == 2

    forget_frame = make_dual_spi_frame(
        DS5_DUAL_MSG_BT_FORGET,
        DS5_DUAL_FLAG_RELIABLE,
        DS5_DUAL_CHANNEL_CTRL,
        DS5_DUAL_PRIORITY_CONTROL,
        0x3348,
        0,
        bytes([DS5_DUAL_FORGET_SAVED_ADDR | DS5_DUAL_FORGET_BONDS]),
    )
    assert int.from_bytes(forget_frame[14:16], "little") == 1
    assert forget_frame[20] == (DS5_DUAL_FORGET_SAVED_ADDR | DS5_DUAL_FORGET_BONDS)


def test_c_source_contract() -> None:
    source = PARSER.read_text(encoding="utf-8")
    output_source = OUTPUT.read_text(encoding="utf-8")
    m61_usb_source = M61_USB.read_text(encoding="utf-8")
    m61_main_source = M61_MAIN.read_text(encoding="utf-8")
    m61_dse_source = M61_DSE.read_text(encoding="utf-8")
    m61_cmake_source = M61_CMAKE.read_text(encoding="utf-8")
    m61_build_source = M61_BUILD_SH.read_text(encoding="utf-8")
    m61_transport_source = M61_TRANSPORT.read_text(encoding="utf-8")
    m61_left_spi_example = M61_LEFT_SPI_EXAMPLE.read_text(encoding="utf-8")
    firmware_manifest_source = FIRMWARE_MANIFEST.read_text(encoding="utf-8")
    check_dual_chip_log_source = CHECK_DUAL_CHIP_LOG.read_text(encoding="utf-8")
    dual_proto_source = DUAL_PROTO.read_text(encoding="utf-8")
    esp32_dual_spi_source = ESP32_DUAL_SPI.read_text(encoding="utf-8")
    esp32_raw_hidp_source = ESP32_RAW_HIDP.read_text(encoding="utf-8")
    esp32_led_status_source = ESP32_LED_STATUS.read_text(encoding="utf-8")
    esp32_dual_defaults = ESP32_DUAL_DEFAULTS.read_text(encoding="utf-8")
    esp32_dual_left_defaults = ESP32_DUAL_LEFT_DEFAULTS.read_text(encoding="utf-8")
    esp32_dual_vspi_defaults = ESP32_DUAL_VSPI_DEFAULTS.read_text(encoding="utf-8")
    dual_wiring_doc = DUAL_WIRING_DOC.read_text(encoding="utf-8")
    required_snippets = [
        "payload_offset = 3;",
        "payload_offset = 2;",
        "payload_offset = 0;",
        "data[0] == DS5_BT_HIDP_INPUT",
        "DS5_BASIC_INPUT_REPORT_ID",
        "state->left_x = p[0];",
        "state->dpad = p[7] & 0x0F;",
        "state->gyro_x = read_i16_le(p + 15);",
        "state->gyro_z = read_i16_le(p + 17);",
        "state->gyro_y = read_i16_le(p + 19);",
        "state->accel_x = read_i16_le(p + 21);",
        "state->battery_percent = p[52] & 0x0F;",
    ]
    for snippet in required_snippets:
        assert snippet in source, f"missing C parser snippet: {snippet}"
    assert "payload_score" not in source

    output_snippets = [
        "#define DS5_OUTPUT_CRC32_SEED 0xA2",
        "s_ds5_set_state_default",
        "DS5_USB_SET_STATE_LEN",
        "apply_usb_set_state",
        "DS5_STATE_ALLOW_RIGHT_TRIGGER_FFB",
        "DS5_STATE_ALLOW_LED_COLOR",
        "copy_set_state",
        "dualsense_output_apply_audio_controls",
        "DS5_OUTPUT_REPORT31_BT_LEN",
        "DS5_OUTPUT_REPORT32_BT_LEN",
        "DS5_OUTPUT_REPORT36_BT_LEN",
        "dualsense_output_make_report36_haptics",
        "dualsense_output_make_report36_audio",
        "dualsense_output_make_report32_audio_status",
        "report[2] = DS5_OUTPUT_AUDIO_TAG;",
        "report[76] = DS5_OUTPUT_HAPTICS_TAG;",
        "report[142] = (uint8_t)(speaker_block_id | 0x80);",
        "report[2] = DS5_OUTPUT_TAG;",
        "report[2] = 0x90;",
        "report[3] = DS5_OUTPUT_SET_STATE_LEN;",
        "fill_crc(report, DS5_OUTPUT_REPORT31_BT_LEN);",
        "fill_crc(report, DS5_OUTPUT_REPORT32_BT_LEN);",
        "fill_crc(report, DS5_OUTPUT_REPORT36_BT_LEN);",
    ]
    for snippet in output_snippets:
        assert snippet in output_source, f"missing C output snippet: {snippet}"

    m61_haptics_snippets = [
        "HAPTICS_RESAMPLE_MODE_WDL_EQUIV",
        "haptics_prev_valid",
        "haptics_phase",
        "haptic_pcm16_to_i8",
        "audio_packet_has_nonzero_speaker",
        "force_zero_haptics",
        "read_i16_le(frame + 4)",
        "read_i16_le(frame + 6)",
        "if (haptics_phase == 0)",
        "haptics_phase >= HAPTICS_DOWNSAMPLE_FACTOR",
    ]
    for snippet in m61_haptics_snippets:
        assert snippet in m61_usb_source, f"missing M61 USB haptics snippet: {snippet}"
    m61_audio_snippets = [
        "#include \"opus/opus.h\"",
        "opus_encoder_get_size",
        "opus_encoder_init",
        "opus_encode(encoder",
        "OPUS_SET_FORCE_CHANNELS",
        "OPUS_SET_MAX_BANDWIDTH",
        "OPUS_BANDWIDTH_MEDIUMBAND",
        "opus_decoder_get_size",
        "opus_decoder_init",
        "opus_decode(decoder",
        "xTaskCreateStatic(audio_codec_task",
        "process_audio_speaker(audio_out_buffer, nbytes)",
        "AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM",
        "resample_speaker_upstream_frame",
        "m61_usb_gamepad_submit_mic_opus",
        "m61_usb_gamepad_audio_mic_enabled",
        "CONFIG_M61_DS5_MIC_DEFAULT_ENABLED",
        "m61_usb_gamepad_take_speaker_opus",
        "AUDIO_IN_STREAM_PACKET_SIZE",
        "audio_mic_usb_nonzero_packets",
        "audio_speaker_encode_us_max",
    ]
    for snippet in m61_audio_snippets:
        assert snippet in m61_usb_source, f"missing M61 USB audio snippet: {snippet}"
    m61_bridge_snippets = [
        "bt_mic_active",
        "host_mic_active && !speaker_active",
        "hidp_last_mic_active != bt_mic_active",
        "maybe_forward_audio_controls",
        "hidp_send_audio_control_state",
        "mic_enabled=%u",
        "USB DualSense registration waits for controller full report",
        "usb_after_ds=%d",
    ]
    for snippet in m61_bridge_snippets:
        assert snippet in m61_main_source, f"missing M61 bridge snippet: {snippet}"
    m61_dse_snippets = [
        "DSE_FEATURE_20_HANDSHAKE_LEN",
        "DSE_PROFILE_REPORT_FIRST",
        "dualsense_feature_fill_crc",
        "m61_ds5_dse_note_feature_report",
        "m61_ds5_dse_note_feature_set",
        "Sent 0x65/0x80 unlock",
        "Unlock wait done; prefetching 0x70-0x7B",
        "Profile snapshot ready",
        "Post-save: re-sent 0x80",
        "Post-save: profile snapshot refetch started",
        "DSE_POST_SAVE_STATUS_POLLS",
    ]
    for snippet in m61_dse_snippets:
        assert snippet in m61_dse_source, f"missing M61 DSE snippet: {snippet}"
    m61_bridge_dse_snippets = [
        "m61_ds5_dse_task(now);",
        "m61_ds5_dse_init(&s_dse_ops);",
        "m61_ds5_dse_note_feature_report(buf->data[1], buf->data + 2, buf->len - 2);",
        "m61_ds5_dse_note_feature_set(report_id);",
        "m61_esp32_transport_set_feature_callback(m61_dse_note_feature_report, NULL);",
        "m61_usb_gamepad_reset_feature_cache();",
    ]
    combined_m61_dse_source = "\n".join([m61_main_source, m61_usb_source, m61_transport_source])
    for snippet in m61_bridge_dse_snippets:
        assert snippet in combined_m61_dse_source, f"missing M61 DSE bridge snippet: {snippet}"
    assert "m61_ds5_dse.c" in m61_cmake_source
    assert "m61_haptics_resampler" not in m61_cmake_source
    assert "resample.cpp" not in m61_cmake_source

    dual_chip_snippets = [
        "DS5_DUAL_HELLO_PAYLOAD_LEN",
        "DS5_DUAL_TIME_SYNC_PAYLOAD_LEN",
        "DS5_DUAL_BT_STATE_PAYLOAD_LEN",
        "DS5_DUAL_STATS_PAYLOAD_LEN",
        "bool ds5_dual_hello_encode",
        "bool ds5_dual_hello_decode",
        "bool ds5_dual_time_sync_encode",
        "bool ds5_dual_time_sync_decode",
        "bool ds5_dual_bt_state_encode",
        "bool ds5_dual_bt_state_decode",
        "bool ds5_dual_ack_encode",
        "bool ds5_dual_ack_decode",
        "bool ds5_dual_stats_encode",
        "bool ds5_dual_stats_decode",
        "set_pending_hello();",
        "set_pending_time_sync(&sync)",
        "set_pending_bt_state(state, timestamp_us)",
        "set_pending_stats();",
        "send_hello();",
        "send_time_sync();",
        "start_time_sync_task();",
        "CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS",
        "CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD",
        "CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS",
        "perform_recovery(reason)",
        "bflb_gpio_reset(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_RESET_PIN)",
        "s_transport.stats.time_sync_failures++",
        "s_transport.stats.recovery_attempts++",
        "s_transport.stats.rx_hello++",
        "s_transport.stats.rx_time_sync++",
        "s_transport.stats.rx_bt_state++",
        "s_transport.stats.rx_stats++",
        "s_stats.hello_rx++",
        "s_stats.time_sync_rx++",
        "s_stats.bt_state_tx++",
        "s_stats.stats_rx++",
        "bt_dualsense_raw_hidp_set_state_callback",
        "bt_dualsense_raw_hidp_connect",
        "bt_dualsense_raw_hidp_disconnect",
        "DS5_RAW_HIDP_AUTO_CONNECT",
        "Raw HIDP auto-connect enabled at startup",
        "clear_target_selection();",
        "Raw HIDP ignoring SDP completion without active target",
        "Raw HIDP staged DualSense address %s for deferred persist",
        "Raw HIDP removed %s from blacklist after successful pair; persist deferred",
        "DS5_DUAL_MSG_BT_CONNECT",
        "DS5_DUAL_MSG_BT_DISCONNECT",
        "DS5_DUAL_MSG_BT_FORGET",
        "uint8_t packet[1 + DS5_OUTPUT_REPORT36_BT_LEN]",
        "DS5_DUAL_MSG_WIRE_TEST",
        "DS5_DUAL_WIRE_TEST_PASS",
        "m61_esp32_transport_connect",
        "m61_esp32_transport_disconnect",
        "m61_esp32_transport_forget",
        "m61_esp32_transport_wire_test",
        "m61_esp32_transport_set_input_callback",
        "dual_chip_input_callback",
        "dual_chip_usb_start_task",
        "DualSense full report seen via ESP32; starting USB composite device",
        "esp32_wire_test HINT",
        "esp32-wire-test",
        "DS5_LED_STATE_WIRE_TEST_PASS",
        "WIRE_HINT_RE",
        "--require-full-report",
        "--require-usb-after-ds",
        "--require-input-reports",
        "--require-audio-rt",
        "--require-mic-opus",
        "--require-no-rt-errors",
        "set_pending_ack(&header, 0)",
        "set_pending_ack(&header, -ENOMEM)",
        "CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH",
        "response_queue_push_front_locked(&item)",
        "response_queue_push_back_locked(&item, replace_existing)",
        "stage_response_for_transaction(&response_item, &response_len)",
        "finish_response_transaction(&response_item, true)",
        "poll_reliable_ack(seq, type)",
        "CONFIG_M61_ESP32_ACK_POLL_COUNT",
        "CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT",
        "s_transport.stats.tx_ack_polls++",
        "s_transport.stats.ack_retries++",
        "s_transport.stats.ack_failures++",
        "s_transport.stats.ack_miss++",
        "(!irq_configured || optional_irq_pin_active())",
        "if (!CONFIG_M61_ESP32_RX_POLL_ENABLE || s_transport.rx_poll_task != NULL)",
        "m61_esp32_transport_request_stats",
        "esp32_peer_stats",
    ]
    combined_dual_chip_source = "\n".join(
        [
            dual_proto_source,
            esp32_dual_spi_source,
            esp32_raw_hidp_source,
            esp32_led_status_source,
            m61_transport_source,
            m61_main_source,
            check_dual_chip_log_source,
        ]
    )
    for snippet in dual_chip_snippets:
        assert snippet in combined_dual_chip_source, f"missing dual-chip snippet: {snippet}"
    esp32_blacklist_snippets = [
        "DS5_NVS_BLACKLIST_KEY",
        "DS5_BLACKLIST_MAX",
        "blacklist_contains",
        "blacklist_bonded_devices",
        "persist_blacklist",
        "load_blacklist",
        "target_origin_allows_blacklist_bypass",
        "RAW_TARGET_SAVED_AUTO",
        "RAW_TARGET_DISCOVERY",
        "RAW_TARGET_MANUAL",
        "saved DualSense %s is blacklisted; scanning for explicit re-pair",
        "should_block_blacklisted_peer",
        "save_bda_if_needed(param->open.rem_bda);",
        "Raw HIDP rejected SSP confirmation from blacklisted",
        "Raw HIDP rejected blacklisted incoming L2CAP open",
        "from blacklist after successful pair",
    ]
    for snippet in esp32_blacklist_snippets:
        assert snippet in esp32_raw_hidp_source, f"missing ESP32 blacklist snippet: {snippet}"

    wiring_snippets = [
        "CONFIG_M61_ESP32_SPI_SCLK_PIN =13",
        "CONFIG_M61_ESP32_SPI_MOSI_PIN =11",
        "CONFIG_M61_ESP32_SPI_MISO_PIN =10",
        "CONFIG_M61_ESP32_SPI_CS_PIN =20",
        "CONFIG_M61_ESP32_READY_PIN =16",
        "CONFIG_M61_ESP32_IRQ_PIN =17",
        "CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT =1",
        "CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS =1000",
        "CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD =8",
        "CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS =5000",
        "CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO=27",
        "CONFIG_DS5_RAW_HIDP_AUTO_CONNECT=y",
        "CONFIG_DS5_DUAL_CHIP_SPI_MOSI_GPIO=26",
        "CONFIG_DS5_DUAL_CHIP_SPI_MISO_GPIO=25",
        "CONFIG_DS5_DUAL_CHIP_SPI_CS_GPIO=33",
        "CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO=32",
        "CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO=13",
        "M61 Left Side + ESP32 Left Side",
        "--pin-profile devkit-left",
        "sdkconfig.dual_chip.devkit_left.defaults",
        "./build.sh all --profile dual-chip-left-spi",
        "\"dual-chip-left-spi\"",
        "BUILD_DIR_NAME=\"build_dual_chip_left_spi\"",
        "esp32-dual-chip-left",
        "m61-dual-chip-left",
        "Do not merge it into `m61/dualsense_hidp_probe/defconfig`",
    ]
    combined_wiring_source = "\n".join(
        [
            m61_left_spi_example,
            esp32_dual_defaults,
            esp32_dual_left_defaults,
            esp32_dual_vspi_defaults,
            dual_wiring_doc,
            m61_build_source,
            firmware_manifest_source,
        ]
    )
    for snippet in wiring_snippets:
        assert snippet in combined_wiring_source, f"missing wiring snippet: {snippet}"


def main() -> int:
    test_vectors()
    test_output_vectors()
    test_dual_chip_spi_vectors()
    test_c_source_contract()
    print("DualSense protocol vectors passed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"DualSense protocol vectors failed: {exc}")
        sys.exit(1)
