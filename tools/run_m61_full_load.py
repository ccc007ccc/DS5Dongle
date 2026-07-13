#!/usr/bin/env python3
"""Run a deterministic DualSense speaker + HD-haptics + HID-output load.

The four USB Audio OUT channels are driven continuously at 48 kHz:
speaker left/right on channels 0/1 and HD haptics left/right on channels 2/3.
A HID SetState report is also sent at a fixed interval with a toggled LED byte
so the USB output and Bluetooth state-report paths cannot coalesce as idle.
"""

from __future__ import annotations

import argparse
import atexit
from pathlib import Path
import threading
import time

import numpy as np
import serial
import sounddevice as sd

from ds5_windows_test_app import (
    DEFAULT_SET_STATE_47,
    DS5_OUTPUT_REPORT_ID,
    WindowsHid,
)


SAMPLE_RATE = 48_000
CHANNELS = 4


def find_audio_device(name_fragment: str) -> tuple[int, str]:
    candidates: list[tuple[int, str, str]] = []
    fragment = name_fragment.casefold()
    for index, device in enumerate(sd.query_devices()):
        name = str(device.get("name", ""))
        if int(device.get("max_output_channels", 0) or 0) < CHANNELS:
            continue
        if fragment not in name.casefold():
            continue
        hostapi = sd.query_hostapis(int(device["hostapi"]))["name"]
        candidates.append((index, name, str(hostapi)))

    if not candidates:
        raise RuntimeError(f"no four-channel output device matching {name_fragment!r}")

    for index, name, hostapi in candidates:
        if "wasapi" in hostapi.casefold():
            return index, f"{name} [{hostapi}]"
    index, name, hostapi = candidates[0]
    return index, f"{name} [{hostapi}]"


def make_audio_block(
    first_frame: int,
    frames: int,
    speaker_amplitude: int,
    haptics_amplitude: int,
) -> bytes:
    positions = np.arange(first_frame, first_frame + frames, dtype=np.float64)
    phase = positions * (2.0 * np.pi / SAMPLE_RATE)
    block = np.empty((frames, CHANNELS), dtype=np.int16)
    block[:, 0] = np.rint(np.sin(phase * 440.0) * speaker_amplitude).astype(np.int16)
    block[:, 1] = np.rint(np.sin(phase * 733.0) * speaker_amplitude).astype(np.int16)
    block[:, 2] = np.rint(np.sin(phase * 160.0) * haptics_amplitude).astype(np.int16)
    block[:, 3] = np.rint(np.sin(phase * 223.0) * haptics_amplitude).astype(np.int16)
    return block.tobytes()


class HidLoadThread(threading.Thread):
    def __init__(self, interval_ms: int) -> None:
        super().__init__(daemon=True)
        self.interval_s = interval_ms / 1000.0
        self.stop_event = threading.Event()
        self.sent = 0
        self.error: Exception | None = None

    def run(self) -> None:
        hid = WindowsHid()
        handle = None
        try:
            infos = hid.find_dualsense_hid()
            if not infos:
                raise RuntimeError("DualSense HID interface MI_03 was not found")
            info = infos[0]
            handle = hid.open_writer(info.path)
            output_len = info.caps.output_len or 48
            deadline = time.perf_counter()
            toggle = False
            while not self.stop_event.is_set():
                payload = bytearray(DEFAULT_SET_STATE_47)
                # Keep native HD haptics selected. Toggle one LED component so
                # every report represents a real state update.
                payload[44] = 0x10 if toggle else 0x20
                payload[46] = 0x20 if toggle else 0x10
                report = bytes([DS5_OUTPUT_REPORT_ID]) + bytes(payload)
                hid.write_output_report(handle, report, output_len)
                self.sent += 1
                toggle = not toggle
                deadline += self.interval_s
                self.stop_event.wait(max(0.0, deadline - time.perf_counter()))
        except Exception as exc:
            self.error = exc
            self.stop_event.set()
        finally:
            if handle is not None:
                try:
                    payload = bytearray(DEFAULT_SET_STATE_47)
                    hid.write_output_report(
                        handle,
                        bytes([DS5_OUTPUT_REPORT_ID]) + bytes(payload),
                        info.caps.output_len or 48,
                    )
                except Exception:
                    pass
                hid.close_handle(handle)


def capture_status(port: str, output: Path) -> None:
    with serial.Serial(port, 115200, timeout=0.2, rtscts=False, dsrdtr=False) as ser:
        ser.reset_input_buffer()
        ser.write(b"ds5 log quiet\r\n")
        ser.flush()
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(b"ds5 status\r\n")
        ser.flush()
        deadline = time.monotonic() + 3.0
        data = bytearray()
        while time.monotonic() < deadline:
            data.extend(ser.read(4096))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)


def set_decoder_benchmark(port: str, enabled: bool) -> None:
    command = b"ds5 decoder-bench on\r\n" if enabled else b"ds5 decoder-bench off\r\n"
    with serial.Serial(port, 115200, timeout=0.2, rtscts=False, dsrdtr=False) as ser:
        ser.reset_input_buffer()
        ser.write(command)
        ser.flush()
        time.sleep(0.4)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=90.0)
    parser.add_argument("--block-frames", type=int, default=480)
    parser.add_argument("--speaker-amplitude", type=int, default=600)
    parser.add_argument("--haptics-amplitude", type=int, default=4000)
    parser.add_argument("--hid-interval-ms", type=int, default=20)
    parser.add_argument("--audio-device", default="DualSense Wireless Controller")
    parser.add_argument("--serial-port", default="COM5")
    parser.add_argument("--status-log", type=Path)
    parser.add_argument(
        "--decoder-bench",
        action="store_true",
        help="enable synthetic mic decode only for this run and always disable it on exit",
    )
    parser.add_argument("--list-devices", action="store_true")
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return 0
    if args.duration <= 0 or args.block_frames <= 0 or args.hid_interval_ms <= 0:
        parser.error("duration, block-frames, and hid-interval-ms must be positive")
    for name, value in (
        ("speaker-amplitude", args.speaker_amplitude),
        ("haptics-amplitude", args.haptics_amplitude),
    ):
        if not 0 <= value <= 32767:
            parser.error(f"{name} must be between 0 and 32767")

    device_index, device_name = find_audio_device(args.audio_device)
    decoder_cleanup_registered = False
    if args.decoder_bench:
        set_decoder_benchmark(args.serial_port, True)
        atexit.register(set_decoder_benchmark, args.serial_port, False)
        decoder_cleanup_registered = True
    print(f"audio device #{device_index}: {device_name}")
    print(
        f"load: duration={args.duration:.1f}s block={args.block_frames} "
        f"speaker_amp={args.speaker_amplitude} haptics_amp={args.haptics_amplitude} "
        f"hid_interval={args.hid_interval_ms}ms "
        f"decoder_bench={'on' if args.decoder_bench else 'off'}"
    )

    before_status_log = None
    if args.status_log is not None:
        before_status_log = args.status_log.with_name(
            f"{args.status_log.stem}_before{args.status_log.suffix}"
        )
        capture_status(args.serial_port, before_status_log)
        print(f"status before: {before_status_log}")

    hid_thread = HidLoadThread(args.hid_interval_ms)
    hid_thread.start()
    first_frame = 0
    total_frames = int(round(args.duration * SAMPLE_RATE))
    started = time.perf_counter()
    try:
        with sd.RawOutputStream(
            samplerate=SAMPLE_RATE,
            channels=CHANNELS,
            dtype="int16",
            device=device_index,
            blocksize=args.block_frames,
        ) as stream:
            while first_frame < total_frames and not hid_thread.stop_event.is_set():
                frames = min(args.block_frames, total_frames - first_frame)
                stream.write(
                    make_audio_block(
                        first_frame,
                        frames,
                        args.speaker_amplitude,
                        args.haptics_amplitude,
                    )
                )
                first_frame += frames
            stream.write(bytes(args.block_frames * CHANNELS * 2))
    finally:
        hid_thread.stop_event.set()
        hid_thread.join(timeout=2.0)

    elapsed = time.perf_counter() - started
    if hid_thread.error is not None:
        raise hid_thread.error
    if first_frame != total_frames:
        raise RuntimeError(f"audio stopped early at {first_frame}/{total_frames} frames")

    print(
        f"completed: frames={first_frame} elapsed={elapsed:.3f}s "
        f"hid_reports={hid_thread.sent}"
    )
    if args.status_log is not None:
        capture_status(args.serial_port, args.status_log)
        print(f"status after: {args.status_log}")
    if decoder_cleanup_registered:
        set_decoder_benchmark(args.serial_port, False)
        atexit.unregister(set_decoder_benchmark)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
