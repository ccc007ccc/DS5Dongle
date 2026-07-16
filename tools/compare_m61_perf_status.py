#!/usr/bin/env python3
"""Compare two `ds5 status` logs and report interval-only M61 metrics."""

from __future__ import annotations

import argparse
from pathlib import Path
import re


ANSI_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


def line_fields(data: bytes, prefix: str) -> dict[str, str]:
    clean = ANSI_RE.sub(b"", data).decode("utf-8", errors="replace")
    for line in clean.splitlines():
        if line.startswith(prefix):
            fields: dict[str, str] = {}
            for token in line[len(prefix) :].strip().split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    fields[key] = value
            return fields
    raise ValueError(f"missing status line: {prefix}")


def optional_line_fields(data: bytes, prefix: str) -> dict[str, str] | None:
    try:
        return line_fields(data, prefix)
    except ValueError:
        return None


def integer(fields: dict[str, str], key: str) -> int:
    return int(fields[key], 0)


def split_integers(fields: dict[str, str], key: str) -> list[int]:
    return [int(value, 0) for value in fields[key].split("/")]


def split_integers_any(fields: dict[str, str], *keys: str) -> list[int]:
    for key in keys:
        if key in fields:
            return split_integers(fields, key)
    raise KeyError(f"none of the status keys are present: {keys!r}")


def split_count_values(fields: dict[str, str], key: str) -> tuple[int, list[int]]:
    count, values = fields[key].split(":", 1)
    return int(count, 0), [int(value, 0) for value in values.split("/")]


def interval_average(before_count: int, before_average: int, after_count: int, after_average: int) -> float:
    delta_count = after_count - before_count
    if delta_count <= 0:
        raise ValueError("sample counter did not increase")
    return (after_count * after_average - before_count * before_average) / delta_count


def max_scope(before_maximum: int, after_maximum: int) -> str:
    """Describe whether a cumulative maximum is known to belong to the interval."""
    if before_maximum == 0 or after_maximum > before_maximum:
        return "interval"
    return "since_boot_upper_bound"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--cpu-mhz", type=float, default=320.0)
    args = parser.parse_args()

    before_data = args.before.read_bytes()
    after_data = args.after.read_bytes()
    speaker0 = line_fields(before_data, "usb_speaker ")
    speaker1 = line_fields(after_data, "usb_speaker ")
    perf0 = line_fields(before_data, "usb_perf ")
    perf1 = line_fields(after_data, "usb_perf ")
    cache0 = line_fields(before_data, "usb_cache ")
    cache1 = line_fields(after_data, "usb_cache ")
    decode0 = optional_line_fields(before_data, "usb_decode_perf ")
    decode1 = optional_line_fields(after_data, "usb_decode_perf ")
    decode_cache0 = optional_line_fields(before_data, "usb_decode_cache ")
    decode_cache1 = optional_line_fields(after_data, "usb_decode_cache ")
    haptics0 = line_fields(before_data, "usb_haptics ")
    haptics1 = line_fields(after_data, "usb_haptics ")
    audio0 = line_fields(before_data, "hidp_audio ")
    audio1 = line_fields(after_data, "hidp_audio ")
    cadence0 = optional_line_fields(before_data, "usb_cadence ")
    cadence1 = optional_line_fields(after_data, "usb_cadence ")
    stage0 = optional_line_fields(before_data, "usb_stage_perf ")
    stage1 = optional_line_fields(after_data, "usb_stage_perf ")
    audio_perf0 = optional_line_fields(before_data, "hidp_audio_perf ")
    audio_perf1 = optional_line_fields(after_data, "hidp_audio_perf ")
    mic0 = optional_line_fields(before_data, "usb_mic ")
    mic1 = optional_line_fields(after_data, "usb_mic ")
    usb_audio0 = optional_line_fields(before_data, "usb_audio ")
    usb_audio1 = optional_line_fields(after_data, "usb_audio ")
    reports0 = optional_line_fields(before_data, "hidp_reports ")
    reports1 = optional_line_fields(after_data, "hidp_reports ")

    samples0 = integer(perf0, "samples")
    samples1 = integer(perf1, "samples")
    cycles0 = split_integers_any(perf0, "cycles_last/avg/max", "cycles")
    cycles1 = split_integers_any(perf1, "cycles_last/avg/max", "cycles")
    encoded0 = integer(speaker0, "encoded")
    encoded1 = integer(speaker1, "encoded")
    encode_us0 = split_integers(speaker0, "enc_us")
    encode_us1 = split_integers(speaker1, "enc_us")

    encode_samples = samples1 - samples0
    encode_us_average = interval_average(
        encoded0, encode_us0[1], encoded1, encode_us1[1]
    )
    encode_cycles_average = interval_average(
        samples0, cycles0[1], samples1, cycles1[1]
    )
    encode_percentiles = split_integers(perf1, "enc_p50/p95/p99")
    print(f"samples={encode_samples}")
    print(
        f"encode_us_avg={encode_us_average:.3f} "
        f"p50/p95/p99={encode_percentiles[0]}/{encode_percentiles[1]}/{encode_percentiles[2]} "
        f"max={encode_us1[2]} "
        f"max_scope={max_scope(encode_us0[2], encode_us1[2])}"
    )
    print(
        f"cycles_avg={encode_cycles_average:.3f} max={cycles1[2]} "
        f"max_scope={max_scope(cycles0[2], cycles1[2])}"
    )
    print(
        "instret_avg="
        f"{interval_average(samples0, integer(perf0, 'instret_avg'), samples1, integer(perf1, 'instret_avg')):.3f}"
    )

    decode_available = all(
        fields is not None
        for fields in (decode0, decode1, decode_cache0, decode_cache1)
    )
    decode_samples0 = integer(decode0, "samples") if decode_available else 0
    decode_samples1 = integer(decode1, "samples") if decode_available else 0
    if decode_available and decode_samples1 > decode_samples0:
        decode_cycles0 = split_integers_any(
            decode0, "cycles_last/avg/max", "cycles"
        )
        decode_cycles1 = split_integers_any(
            decode1, "cycles_last/avg/max", "cycles"
        )
        decode_us0 = split_integers_any(
            decode0, "dec_us_last/avg/max", "dec_us"
        )
        decode_us1 = split_integers_any(
            decode1, "dec_us_last/avg/max", "dec_us"
        )
        decode_samples = decode_samples1 - decode_samples0
        decode_us_average = interval_average(
            decode_samples0, decode_us0[1], decode_samples1, decode_us1[1]
        )
        decode_cycles_average = interval_average(
            decode_samples0,
            decode_cycles0[1],
            decode_samples1,
            decode_cycles1[1],
        )
        decode_percentiles = split_integers(decode1, "dec_p50/p95/p99")
        print(f"decode_samples={decode_samples}")
        print(
            "decode_us_avg="
            f"{decode_us_average:.3f} "
            f"p50/p95/p99={decode_percentiles[0]}/{decode_percentiles[1]}/{decode_percentiles[2]} "
            f"max={decode_us1[2]} "
            f"max_scope={max_scope(decode_us0[2], decode_us1[2])}"
        )
        print(
            "decode_cycles_avg="
            f"{decode_cycles_average:.3f} max={decode_cycles1[2]} "
            f"max_scope={max_scope(decode_cycles0[2], decode_cycles1[2])}"
        )
        print(
            "decode_instret_avg="
            f"{interval_average(decode_samples0, integer(decode0, 'instret_avg'), decode_samples1, integer(decode1, 'instret_avg')):.3f}"
        )
        print(
            f"decode_bench_frames={integer(decode1, 'bench_frames') - integer(decode0, 'bench_frames')} "
            f"decode_bench_errors={integer(decode1, 'bench_errors') - integer(decode0, 'bench_errors')}"
        )
        total_cycles_average = encode_cycles_average + decode_cycles_average
        print(
            f"codec_cycles_avg={total_cycles_average:.3f} "
            f"cpu_10ms={total_cycles_average / (args.cpu_mhz * 10000.0) * 100.0:.3f}%"
        )

    for key, label in (
        ("ic_access/miss/ppm", "icache"),
        ("dc_read/miss/ppm", "dcache"),
    ):
        values0 = split_integers(cache0, key)
        values1 = split_integers(cache1, key)
        access = interval_average(samples0, values0[0], samples1, values1[0])
        misses = interval_average(samples0, values0[1], samples1, values1[1])
        ppm = misses * 1_000_000.0 / access if access else 0.0
        print(f"{label}_access_avg={access:.3f} {label}_miss_avg={misses:.3f} {label}_miss_ppm={ppm:.1f}")

    if decode_available and decode_samples1 > decode_samples0:
        for key, label in (
            ("ic_access/miss/ppm", "decode_icache"),
            ("dc_read/miss/ppm", "decode_dcache"),
        ):
            values0 = split_integers(decode_cache0, key)
            values1 = split_integers(decode_cache1, key)
            access = interval_average(decode_samples0, values0[0], decode_samples1, values1[0])
            misses = interval_average(decode_samples0, values0[1], decode_samples1, values1[1])
            ppm = misses * 1_000_000.0 / access if access else 0.0
            print(
                f"{label}_access_avg={access:.3f} "
                f"{label}_miss_avg={misses:.3f} {label}_miss_ppm={ppm:.1f}"
            )

    for fields0, fields1, keys in (
        (speaker0, speaker1, ("qdrop", "odrop", "cancel", "enc_err")),
        (haptics0, haptics1, ("queued", "nonzero", "qdrop", "deadline")),
        (audio0, audio1, ("sent", "errors", "stale", "noconn")),
    ):
        print(" ".join(f"{key}={integer(fields1, key) - integer(fields0, key)}" for key in keys))

    if cadence0 is not None and cadence1 is not None:
        frame_counts0 = split_integers(cadence0, "pkt_frames48/49/other")
        frame_counts1 = split_integers(cadence1, "pkt_frames48/49/other")
        print(
            "usb_frames48/49/other="
            + "/".join(str(after - before) for before, after in zip(frame_counts0, frame_counts1))
        )
        for key, label in (
            ("pkt_interval", "usb_packet_interval_us_avg"),
            ("epoch_interval", "usb_epoch_interval_us_avg"),
        ):
            count0, values0 = split_count_values(cadence0, key)
            count1, values1 = split_count_values(cadence1, key)
            if count1 > count0:
                print(
                    f"{label}="
                    f"{interval_average(count0, values0[1], count1, values1[1]):.3f}"
                )

    if stage0 is not None and stage1 is not None:
        for key, label in (
            ("ingress_work", "ingress_work_us_avg"),
            ("resample", "resample_us_avg"),
        ):
            count0, values0 = split_count_values(stage0, key)
            count1, values1 = split_count_values(stage1, key)
            if count1 > count0:
                print(
                    f"{label}="
                    f"{interval_average(count0, values0[1], count1, values1[1]):.3f}"
                )

    if audio_perf0 is not None and audio_perf1 is not None:
        for key in ("alloc", "build", "send", "total", "pair_age", "report_interval"):
            count0, values0 = split_count_values(audio_perf0, key)
            count1, values1 = split_count_values(audio_perf1, key)
            if count1 > count0:
                print(
                    f"bt_{key}_us_avg="
                    f"{interval_average(count0, values0[1], count1, values1[1]):.3f}"
                )

    if mic0 is not None and mic1 is not None:
        mic_keys = (
            "opus",
            "opus_nz",
            "odrop",
            "decoded",
            "dec_err",
            "pcm_bytes",
            "pcm_nz",
            "usb_nz_pkts",
            "usb_nz_bytes",
            "underflow",
        )
        print(
            " ".join(
                f"mic_{key}={integer(mic1, key) - integer(mic0, key)}"
                for key in mic_keys
            )
        )
        print(
            f"mic_opus_qdepth={integer(mic1, 'oqdepth')} "
            f"mic_pcm_ring={integer(mic1, 'ring')}"
        )
        decoded = integer(mic1, "decoded") - integer(mic0, "decoded")
        pcm_bytes = integer(mic1, "pcm_bytes") - integer(mic0, "pcm_bytes")
        produced_packets = pcm_bytes // 192
        expected_packets = decoded * 10
        print(
            f"mic_pcm_packets={produced_packets}/{expected_packets} "
            f"mic_pcm_packet_shortfall={max(0, expected_packets - produced_packets)}"
        )

    if usb_audio0 is not None and usb_audio1 is not None:
        usb_in_packets = integer(usb_audio1, "in_pkts") - integer(
            usb_audio0, "in_pkts"
        )
        mic_underflows = (
            integer(mic1, "underflow") - integer(mic0, "underflow")
            if mic0 is not None and mic1 is not None
            else 0
        )
        print(
            f"usb_in_pkts={usb_in_packets} "
            f"usb_in_bytes={integer(usb_audio1, 'in_bytes') - integer(usb_audio0, 'in_bytes')}"
        )
        if usb_in_packets > 0:
            print(
                f"mic_usb_underflow_rate={mic_underflows / usb_in_packets * 100.0:.3f}%"
            )

    if reports0 is not None and reports1 is not None:
        print(
            "bt_mic_audio_reports="
            f"{integer(reports1, 'mic_audio') - integer(reports0, 'mic_audio')}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
