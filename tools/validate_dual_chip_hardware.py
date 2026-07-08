#!/usr/bin/env python3
"""Capture and check an M61 + ESP32 dual-chip bring-up log."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time

import check_dual_chip_log


def write_command(ser, log, command: str, echo_stdout: bool) -> None:
    marker = f"\r\n# host: {command}\r\n".encode("ascii")
    log.write(marker)
    log.flush()
    if echo_stdout:
        sys.stdout.buffer.write(marker)
        sys.stdout.buffer.flush()
    ser.write(command.encode("ascii") + b"\r\n")
    ser.flush()


def capture_status_log(
    port: str,
    baud: int,
    output: Path,
    duration_s: float,
    status_interval_s: float,
    run_wire_test: bool,
    echo_stdout: bool,
) -> int:
    import serial

    end_time = time.monotonic() + duration_s
    next_status = time.monotonic()

    with serial.Serial(port, baudrate=baud, timeout=0.2, rtscts=False, dsrdtr=False) as ser:
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.05)
        ser.reset_input_buffer()

        print(f"Capturing {port} at {baud} for {duration_s:.1f}s -> {output}")
        with output.open("wb") as log:
            write_command(ser, log, "ds5 log quiet", echo_stdout)
            if run_wire_test:
                write_command(ser, log, "ds5 esp32-wire-test", echo_stdout)
            while time.monotonic() < end_time:
                now = time.monotonic()
                if now >= next_status:
                    write_command(ser, log, "ds5 status", echo_stdout)
                    next_status = now + status_interval_s

                data = ser.read(4096)
                if not data:
                    continue
                if echo_stdout:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                log.write(data)
                log.flush()

    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", help="M61 serial port, for example COM5")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="M61 probe console baud")
    parser.add_argument("-o", "--output", type=Path, default=Path("m61_dual_chip_validation.log"))
    parser.add_argument("--duration", type=float, default=30.0, help="capture duration in seconds")
    parser.add_argument("--status-interval", type=float, default=2.0, help="seconds between ds5 status commands")
    parser.add_argument("--no-stdout", action="store_true", help="write the capture file without echoing serial data")
    parser.add_argument("--no-wire-test", action="store_true", help="do not send ds5 esp32-wire-test before status polling")
    parser.add_argument("--spi-only", action="store_true", help="only require SPI handshake, STATS, and ACK")
    parser.add_argument("--require-full-report", action="store_true", help="require DualSense full report in ds5 status")
    parser.add_argument("--require-usb-after-ds", action="store_true", help="require USB composite startup after full report")
    parser.add_argument("--require-input-reports", action="store_true", help="require ESP32-to-M61 input reports")
    parser.add_argument("--require-audio-rt", action="store_true", help="require realtime 0x39 audio/haptics reports")
    parser.add_argument("--require-mic-opus", action="store_true", help="require DualSense mic Opus packets")
    parser.add_argument("--require-no-rt-errors", action="store_true", help="require zero realtime deadline/TX errors")
    parser.add_argument("--require-boot-banners", action="store_true", help="require one-shot M61 boot/init banners in the capture")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    check_args = [
        "--require-stats",
        "--require-ack",
    ]
    if args.require_boot_banners:
        check_args.extend(["--require-m61-mode", "--require-transport-ready"])
    if not args.no_wire_test:
        check_args.append("--require-wire-test")
    if not args.spi_only:
        check_args.extend(["--require-credit", "--require-bt-state"])
    if args.require_full_report:
        check_args.append("--require-full-report")
    if args.require_usb_after_ds:
        check_args.append("--require-usb-after-ds")
    if args.require_input_reports:
        check_args.append("--require-input-reports")
    if args.require_audio_rt:
        check_args.append("--require-audio-rt")
    if args.require_mic_opus:
        check_args.append("--require-mic-opus")
    if args.require_no_rt_errors:
        check_args.append("--require-no-rt-errors")

    if args.self_test:
        return check_dual_chip_log.main(["--self-test"])

    if args.port is None:
        parser.error("--port is required unless --self-test is used")
    if args.duration <= 0:
        parser.error("--duration must be positive")
    if args.status_interval <= 0:
        parser.error("--status-interval must be positive")

    print("Dual-chip hardware validation capture starting.")
    print("This only uses the M61 serial shell; it does not flash or reset either board.")
    capture_result = capture_status_log(
        args.port,
        args.baud,
        args.output,
        args.duration,
        args.status_interval,
        not args.no_wire_test,
        not args.no_stdout,
    )
    if capture_result != 0:
        return capture_result

    return check_dual_chip_log.main([str(args.output), *check_args])


if __name__ == "__main__":
    raise SystemExit(main())
