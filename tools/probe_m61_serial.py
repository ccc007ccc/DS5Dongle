#!/usr/bin/env python3
"""Probe an M61 serial port and classify the DS5Dongle helper firmware.

The probe is intentionally non-flashing: it does not toggle M61 boot straps and
does not request ESP32 download mode. It sends status commands understood by the
two development firmwares in this repository and classifies the response.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial
from serial.tools import list_ports


DEFAULT_BAUDS = (115200, 460800, 2000000)

BRIDGE_MARKERS = (
    b"M61 bridge status",
    b"M61 ESP32 bridge commands",
    b"M61ESP32BRIDGE",
    b"OK boot",
    b"OK run",
    b"OK reset",
)

HIDP_PROBE_MARKERS = (
    b"M61 DualSense HIDP probe ready",
    b"bt_ready=",
    b"auto=",
    b"hid_control=",
    b"HIDP control",
    b"ds5 auto",
)

UNKNOWN_BOOT_MARKERS = (
    b"Booting",
    b"shell",
    b"bouffalo",
    b"bl616",
    b"bl618",
)


def available_ports() -> list[str]:
    return [port.device for port in list_ports.comports()]


def read_until_deadline(ser: serial.Serial, deadline: float) -> bytes:
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = ser.read(4096)
        if data:
            chunks.append(data)
        else:
            time.sleep(0.02)
    return b"".join(chunks)


def send_and_read(ser: serial.Serial, command: str, read_ms: int) -> bytes:
    ser.write(command.encode("ascii") + b"\n")
    ser.flush()
    return read_until_deadline(ser, time.monotonic() + read_ms / 1000.0)


def classify(output: bytes) -> str:
    lowered = output.lower()
    if any(marker in output for marker in BRIDGE_MARKERS):
        return "bridge"
    if any(marker in output for marker in HIDP_PROBE_MARKERS):
        return "hidp-probe"
    if any(marker.lower() in lowered for marker in UNKNOWN_BOOT_MARKERS):
        return "unknown-booting"
    if output:
        return "unknown-responsive"
    return "no-response"


def probe_port(port: str, baud: int, read_ms: int, listen_ms: int) -> tuple[str, bytes]:
    with serial.Serial(port, baudrate=baud, timeout=0.05, rtscts=False, dsrdtr=False) as ser:
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.05)

        output = read_until_deadline(ser, time.monotonic() + listen_ms / 1000.0)
        ser.reset_input_buffer()

        bridge_output = send_and_read(ser, "~m61 status", read_ms)
        output += bridge_output
        if classify(output) == "bridge":
            return "bridge", output

        hidp_output = send_and_read(ser, "ds5 status", read_ms)
        output += hidp_output
        return classify(output), output


def exit_code_for_kind(kind: str) -> int:
    if kind == "no-response":
        return 2
    if kind.startswith("unknown"):
        return 3
    return 0


def kind_priority(kind: str) -> int:
    if kind in ("bridge", "hidp-probe"):
        return 3
    if kind == "unknown-responsive":
        return 2
    if kind == "unknown-booting":
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", help="M61 host serial port, for example COM5")
    parser.add_argument(
        "-b",
        "--baud",
        action="append",
        type=int,
        help=(
            "serial baud to probe; repeatable. If omitted, probes "
            f"{', '.join(str(baud) for baud in DEFAULT_BAUDS)}"
        ),
    )
    parser.add_argument("--read-ms", type=int, default=1000, help="read window after each command")
    parser.add_argument("--listen-ms", type=int, default=300, help="initial passive listen window")
    parser.add_argument("--dump", action="store_true", help="print raw received bytes")
    args = parser.parse_args(argv)

    if args.port is None:
        ports = available_ports()
        if not ports:
            print("No serial ports found.", file=sys.stderr)
            return 1
        if len(ports) > 1:
            print("Multiple serial ports found; pass -p explicitly:", file=sys.stderr)
            for port in ports:
                print(f"  {port}", file=sys.stderr)
            return 1
        args.port = ports[0]

    bauds = args.baud or list(DEFAULT_BAUDS)
    best_kind = "no-response"
    best_output = b""
    successful_probes = 0
    serial_error: serial.SerialException | None = None

    for baud in bauds:
        try:
            kind, output = probe_port(args.port, baud, args.read_ms, args.listen_ms)
        except serial.SerialException as exc:
            serial_error = exc
            print(f"M61 serial probe failed at {baud}: {exc}", file=sys.stderr)
            continue

        successful_probes += 1
        print(f"M61 serial probe: port={args.port} baud={baud} kind={kind}")
        if args.dump and output:
            sys.stdout.buffer.write(output)
            if not output.endswith(b"\n"):
                sys.stdout.buffer.write(b"\n")
            sys.stdout.buffer.flush()

        if kind_priority(kind) > kind_priority(best_kind):
            best_kind = kind
            best_output = output

        if kind in ("bridge", "hidp-probe"):
            break

    if successful_probes == 0 and serial_error is not None and not best_output:
        return 1
    return exit_code_for_kind(best_kind)


if __name__ == "__main__":
    raise SystemExit(main())
