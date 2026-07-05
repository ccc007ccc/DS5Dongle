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
M61_CMAKE = ROOT / "m61" / "dualsense_hidp_probe" / "CMakeLists.txt"

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


def test_c_source_contract() -> None:
    source = PARSER.read_text(encoding="utf-8")
    output_source = OUTPUT.read_text(encoding="utf-8")
    m61_usb_source = M61_USB.read_text(encoding="utf-8")
    m61_cmake_source = M61_CMAKE.read_text(encoding="utf-8")
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
        "opus_decoder_get_size",
        "opus_decoder_init",
        "opus_decode(decoder",
        "xTaskCreateStatic(audio_codec_task",
        "process_audio_speaker(audio_out_buffer, nbytes)",
        "m61_usb_gamepad_submit_mic_opus",
        "m61_usb_gamepad_take_speaker_opus",
        "AUDIO_IN_STREAM_PACKET_SIZE",
    ]
    for snippet in m61_audio_snippets:
        assert snippet in m61_usb_source, f"missing M61 USB audio snippet: {snippet}"
    assert "m61_haptics_resampler" not in m61_cmake_source
    assert "resample.cpp" not in m61_cmake_source


def main() -> int:
    test_vectors()
    test_output_vectors()
    test_c_source_contract()
    print("DualSense protocol vectors passed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"DualSense protocol vectors failed: {exc}")
        sys.exit(1)
