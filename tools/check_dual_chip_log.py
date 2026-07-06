#!/usr/bin/env python3
"""Check M61 + ESP32 dual-chip runtime logs."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import io
from pathlib import Path
import re
import sys


M61_SPI_RE = re.compile(r"^esp32_spi\s+(?P<body>.*)$", re.MULTILINE)
M61_BT_RE = re.compile(r"^esp32_bt\s+(?P<body>.*)$", re.MULTILINE)
M61_PEER_STATS_RE = re.compile(r"^esp32_peer_stats\s+(?P<body>.*)$", re.MULTILINE)
M61_AUTO_RE = re.compile(r"^auto=(?P<body>.*)$", re.MULTILINE)
WIRE_TEST_RE = re.compile(r"^esp32_wire_test result=(?P<result>PASS|FAIL)\s+(?P<body>.*)$", re.MULTILINE)
WIRE_HINT_RE = re.compile(r"^esp32_wire_test HINT (?P<hint>.*)$", re.MULTILINE)
WIRE_WARN_RE = re.compile(r"^esp32_wire_test WARN (?P<warn>.*)$", re.MULTILINE)
DS5_DUAL_BT_STATE_FULL_REPORT = 0x20


SAMPLE_LOG = "\n".join([
    "M61 dual-chip mode: local Classic BT host is not started; ESP32 owns HIDP transport",
    "M61 ESP32 dual-chip SPI transport ready enable=1 pins=13/11/10/20 ready_pin=16 irq_pin=17 reset_pin=255 rx_poll=1 tsync_ms=1000 recov_threshold=8 recov_cooldown_ms=5000 err=0 hello=1 time_sync=1 sync_fail=0 sync_valid=1 rtt_us=320 offset_us=42 peer_role=2 peer_ver=1 peer_mtu=532 peer_caps=0x0000003e",
    "esp32_spi ready=1 tx=12 bytes=1024 hello_tx=1 tsync_tx=4 tsync_fail=0 tsync_age_ms=20 recov=0 recov_ok=0 recov_fail=0 recov_skip=0 recov_suppress=0 recov_consec=0 recov_reason=0 audio_rt=5 reports=2 fget=1 fset=1 bt_conn=1 bt_disc=0 rst=0 stats_req=1 rx=11 rx_bytes=900 hello_rx=1 tsync_rx=4 stats_rx=1 sync=1 rtt_us=310 offset_us=41 ack=3 ack_poll=4 ack_retry=0 ack_fail=0 ack_miss=0 ack_err=0 ack_seq=7 ack_type=13 ack_status=0 credit=2 free=3/4 bt=1 peer_role=2 peer_ver=1 peer_mtu=532 peer_payload=512 peer_q=4 peer_caps=0x0000003e peer_drop=0 peer_txerr=0 peer_last=0 ferr=0 crc=0 deadline=0 drop_old=0 not_ready=0 seq=12 last_err=0",
    "auto=1 sequence=1 security=1 sdp=1 hidp=1 full_report=1 usb_after_ds=1 bringup=1/8",
    "esp32_bt state_rx=1 flags=0x0000003f seq=4 err=0 rssi=-45 bringup=1 reconnect_fail=0 mtu=672/672 bda=aa:bb:cc:dd:ee:ff",
    "esp32_peer_stats role=2 ver=1 uptime_us=123456 spi_rx=12 spi_tx=11 spi_crc=0 spi_drop=0 tx31=1 tx32=1 tx36=5 fget=1 fset=1 txerr=0 deadline36=0 rx_input=20 rx_mic=4 rx_feature=1 ack_tx=3 ack_drop=0 credit_tx=2 bt_conn_rx=1 bt_disc_rx=0",
    "esp32_wire_test result=PASS ready=1 hello=2 tsync=5 stats=2 ack=5 irq=1 peer_role=2 peer_stats_role=2",
])


def self_test() -> int:
    args = argparse.Namespace(
        require_m61_mode=True,
        require_transport_ready=True,
        require_credit=True,
        require_stats=True,
        require_ack=True,
        require_bt_state=True,
        require_wire_test=True,
        require_full_report=True,
        require_usb_after_ds=True,
        require_input_reports=True,
        require_audio_rt=True,
        require_mic_opus=True,
        require_no_rt_errors=True,
    )
    if check_log(SAMPLE_LOG, args) != 0:
        return 1

    missing_peer = "\n".join(
        line for line in SAMPLE_LOG.splitlines()
        if not line.startswith("esp32_peer_stats ")
    )
    with redirect_stdout(io.StringIO()):
        missing_peer_result = check_log(missing_peer, args)
    if missing_peer_result == 0:
        print("Dual-chip log self-test failed: missing peer stats unexpectedly passed")
        return 1

    deadline_failure = SAMPLE_LOG.replace("deadline36=0", "deadline36=1")
    with redirect_stdout(io.StringIO()):
        deadline_failure_result = check_log(deadline_failure, args)
    if deadline_failure_result == 0:
        print("Dual-chip log self-test failed: deadline36=1 unexpectedly passed")
        return 1

    stale_full_report = SAMPLE_LOG.replace("flags=0x0000003f", "flags=0x0000001f")
    with redirect_stdout(io.StringIO()):
        stale_full_report_result = check_log(stale_full_report, args)
    if stale_full_report_result == 0:
        print("Dual-chip log self-test failed: BT_STATE without full-report unexpectedly passed")
        return 1

    print("Dual-chip log checker self-test passed.")
    return 0


def parse_key_values(body: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for part in body.split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        result[key] = value.rstrip(",")
    return result


def parse_int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        if value.lower().startswith("0x"):
            return int(value, 16)
        if "/" in value:
            return int(value.split("/", 1)[0])
        return int(value, 10)
    except ValueError:
        return None


def latest_match(regex: re.Pattern[str], text: str) -> dict[str, str]:
    matches = list(regex.finditer(text))
    if not matches:
        return {}
    return parse_key_values(matches[-1].group("body"))


def require_counter(
    failures: list[str],
    values: dict[str, str],
    key: str,
    minimum: int,
    label: str,
) -> None:
    value = parse_int(values.get(key))
    if value is None or value < minimum:
        failures.append(f"{label}: expected {key}>={minimum}, got {values.get(key, '<missing>')}")


def require_zero(
    failures: list[str],
    values: dict[str, str],
    key: str,
    label: str,
) -> None:
    value = parse_int(values.get(key))
    if value is None:
        failures.append(f"{label}: expected {key}=0, got {values.get(key, '<missing>')}")
    elif value != 0:
        failures.append(f"{label}: expected {key}=0, got {values.get(key)}")


def check_log(text: str, args: argparse.Namespace) -> int:
    spi = latest_match(M61_SPI_RE, text)
    auto = latest_match(M61_AUTO_RE, text)
    bt = latest_match(M61_BT_RE, text)
    peer = latest_match(M61_PEER_STATS_RE, text)
    wire_matches = list(WIRE_TEST_RE.finditer(text))
    wire_result = wire_matches[-1].group("result") if wire_matches else None
    wire_passed = wire_result == "PASS"
    wire_hints = [match.group("hint") for match in WIRE_HINT_RE.finditer(text)]
    wire_warnings = [match.group("warn") for match in WIRE_WARN_RE.finditer(text)]
    realtime_required = (
        args.require_audio_rt or
        args.require_mic_opus or
        args.require_input_reports or
        args.require_no_rt_errors
    )

    failures: list[str] = []
    if args.require_m61_mode and "M61 dual-chip mode:" not in text:
        failures.append("missing M61 dual-chip mode banner")
    if args.require_transport_ready and "M61 ESP32 dual-chip SPI transport ready" not in text:
        failures.append("missing ready M61 transport init line")
    if args.require_wire_test and wire_result != "PASS":
        failures.append(f"wire test did not pass: result={wire_result or '<missing>'}")
    if args.require_full_report:
        if not auto:
            failures.append("missing ds5 status auto/full_report line")
        else:
            require_counter(failures, auto, "full_report", 1, "DualSense full report")
        if not bt:
            failures.append("missing ds5 status esp32_bt line for full-report verification")
        else:
            bt_flags = parse_int(bt.get("flags"))
            if bt_flags is None or (bt_flags & DS5_DUAL_BT_STATE_FULL_REPORT) == 0:
                failures.append(
                    "ESP32 BT_STATE does not report full-report mode: "
                    f"flags={bt.get('flags', '<missing>')}"
                )
    if args.require_usb_after_ds:
        if not auto:
            failures.append("missing ds5 status auto/usb_after_ds line")
        else:
            require_counter(failures, auto, "usb_after_ds", 1, "USB started after DualSense full report")

    if not spi:
        failures.append("missing ds5 status esp32_spi line")
    else:
        require_counter(failures, spi, "ready", 1, "M61 transport ready")
        require_counter(failures, spi, "hello_rx", 1, "HELLO response")
        require_counter(failures, spi, "tsync_rx", 1, "TIME_SYNC response")
        require_counter(failures, spi, "sync", 1, "TIME_SYNC valid")
        require_counter(failures, spi, "peer_role", 2, "ESP32 peer role")
        require_counter(failures, spi, "peer_ver", 1, "SPI protocol version")
        if args.require_credit:
            require_counter(failures, spi, "credit", 1, "FLOW_CREDIT response")
        if args.require_stats:
            require_counter(failures, spi, "stats_rx", 1, "STATS response")
        if args.require_ack:
            require_counter(failures, spi, "ack", 1, "reliable ACK response")
        if not wire_passed and parse_int(spi.get("crc")) not in (0, None):
            failures.append(f"SPI CRC errors present: crc={spi.get('crc')}")
        if not wire_passed and parse_int(spi.get("ferr")) not in (0, None):
            failures.append(f"SPI frame errors present: ferr={spi.get('ferr')}")
        if not wire_passed and parse_int(spi.get("ack_fail")) not in (0, None):
            failures.append(f"ACK failures present: ack_fail={spi.get('ack_fail')}")
        if args.require_audio_rt:
            require_counter(failures, spi, "audio_rt", 1, "M61 realtime 0x36 TX")
        if args.require_no_rt_errors:
            require_zero(failures, spi, "deadline", "M61 realtime deadline miss")
            require_zero(failures, spi, "not_ready", "M61 transport not-ready drops")

    if args.require_bt_state:
        if not bt:
            failures.append("missing ds5 status esp32_bt line")
        else:
            require_counter(failures, bt, "state_rx", 1, "BT_STATE response")
            require_counter(failures, bt, "flags", 1, "BT state flags")

    if args.require_stats or realtime_required:
        if not peer:
            failures.append("missing ds5 status esp32_peer_stats line")
        else:
            require_counter(failures, peer, "role", 2, "ESP32 stats role")
            require_counter(failures, peer, "ver", 1, "ESP32 stats protocol version")
            if not wire_passed and parse_int(peer.get("spi_crc")) not in (0, None):
                failures.append(f"ESP32 SPI CRC errors present: spi_crc={peer.get('spi_crc')}")
            if realtime_required and parse_int(peer.get("txerr")) not in (0, None):
                failures.append(f"ESP32 HIDP TX errors present: txerr={peer.get('txerr')}")
            if args.require_audio_rt:
                require_counter(failures, peer, "tx36", 1, "ESP32 HIDP 0x36 TX")
            if args.require_mic_opus:
                require_counter(failures, peer, "rx_mic", 1, "ESP32 mic Opus RX")
            if args.require_input_reports:
                require_counter(failures, peer, "rx_input", 1, "ESP32 input reports")
            if args.require_no_rt_errors:
                require_zero(failures, peer, "deadline36", "ESP32 0x36 deadline miss")
                require_zero(failures, peer, "txerr", "ESP32 HIDP TX errors")

    print("Dual-chip log summary:")
    print(f"  m61_mode={'yes' if 'M61 dual-chip mode:' in text else 'no'}")
    print(f"  spi_ready={spi.get('ready', '<missing>') if spi else '<missing>'}")
    print(f"  hello_rx={spi.get('hello_rx', '<missing>') if spi else '<missing>'}")
    print(f"  tsync_rx={spi.get('tsync_rx', '<missing>') if spi else '<missing>'}")
    print(f"  stats_rx={spi.get('stats_rx', '<missing>') if spi else '<missing>'}")
    print(f"  full_report={auto.get('full_report', '<missing>') if auto else '<missing>'}")
    print(f"  usb_after_ds={auto.get('usb_after_ds', '<missing>') if auto else '<missing>'}")
    print(f"  credit={spi.get('credit', '<missing>') if spi else '<missing>'}")
    print(f"  bt_state_rx={bt.get('state_rx', '<missing>') if bt else '<missing>'}")
    print(f"  peer_stats={'yes' if peer else 'no'}")
    print(f"  tx36={peer.get('tx36', '<missing>') if peer else '<missing>'}")
    print(f"  deadline36={peer.get('deadline36', '<missing>') if peer else '<missing>'}")
    print(f"  rx_input={peer.get('rx_input', '<missing>') if peer else '<missing>'}")
    print(f"  rx_mic={peer.get('rx_mic', '<missing>') if peer else '<missing>'}")
    print(f"  wire_test={wire_result or '<missing>'}")
    if wire_warnings:
        print("  wire_warnings=" + " | ".join(wire_warnings[-3:]))

    if failures:
        print("Dual-chip log check failed:")
        for failure in failures:
            print(f"  - {failure}")
        if wire_hints:
            print("Wire-test hints from M61:")
            for hint in wire_hints[-5:]:
                print(f"  - {hint}")
        return 1

    print("Dual-chip log check passed.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", type=Path, help="Captured M61 console log containing ds5 status")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--require-m61-mode", action="store_true")
    parser.add_argument("--require-transport-ready", action="store_true")
    parser.add_argument("--require-credit", action="store_true")
    parser.add_argument("--require-stats", action="store_true")
    parser.add_argument("--require-ack", action="store_true")
    parser.add_argument("--require-bt-state", action="store_true")
    parser.add_argument("--require-wire-test", action="store_true")
    parser.add_argument("--require-full-report", action="store_true")
    parser.add_argument("--require-usb-after-ds", action="store_true")
    parser.add_argument("--require-input-reports", action="store_true")
    parser.add_argument("--require-audio-rt", action="store_true")
    parser.add_argument("--require-mic-opus", action="store_true")
    parser.add_argument("--require-no-rt-errors", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.log is None:
        parser.error("log file is required unless --self-test is used")
    if not args.log.is_file():
        parser.error(f"log file not found: {args.log}")
    text = args.log.read_text(encoding="utf-8", errors="replace")
    return check_log(text, args)


if __name__ == "__main__":
    sys.exit(main())
