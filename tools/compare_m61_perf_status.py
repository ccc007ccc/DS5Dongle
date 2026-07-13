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


def integer(fields: dict[str, str], key: str) -> int:
    return int(fields[key], 0)


def split_integers(fields: dict[str, str], key: str) -> list[int]:
    return [int(value, 0) for value in fields[key].split("/")]


def interval_average(before_count: int, before_average: int, after_count: int, after_average: int) -> float:
    delta_count = after_count - before_count
    if delta_count <= 0:
        raise ValueError("sample counter did not increase")
    return (after_count * after_average - before_count * before_average) / delta_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()

    before_data = args.before.read_bytes()
    after_data = args.after.read_bytes()
    speaker0 = line_fields(before_data, "usb_speaker ")
    speaker1 = line_fields(after_data, "usb_speaker ")
    perf0 = line_fields(before_data, "usb_perf ")
    perf1 = line_fields(after_data, "usb_perf ")
    cache0 = line_fields(before_data, "usb_cache ")
    cache1 = line_fields(after_data, "usb_cache ")
    decode0 = line_fields(before_data, "usb_decode_perf ")
    decode1 = line_fields(after_data, "usb_decode_perf ")
    decode_cache0 = line_fields(before_data, "usb_decode_cache ")
    decode_cache1 = line_fields(after_data, "usb_decode_cache ")
    haptics0 = line_fields(before_data, "usb_haptics ")
    haptics1 = line_fields(after_data, "usb_haptics ")
    audio0 = line_fields(before_data, "hidp_audio ")
    audio1 = line_fields(after_data, "hidp_audio ")

    samples0 = integer(perf0, "samples")
    samples1 = integer(perf1, "samples")
    cycles0 = split_integers(perf0, "cycles")
    cycles1 = split_integers(perf1, "cycles")
    encoded0 = integer(speaker0, "encoded")
    encoded1 = integer(speaker1, "encoded")
    encode_us0 = split_integers(speaker0, "enc_us")
    encode_us1 = split_integers(speaker1, "enc_us")

    print(f"samples={samples1 - samples0}")
    print(f"encode_us_avg={interval_average(encoded0, encode_us0[1], encoded1, encode_us1[1]):.3f}")
    print(f"cycles_avg={interval_average(samples0, cycles0[1], samples1, cycles1[1]):.3f}")
    print(
        "instret_avg="
        f"{interval_average(samples0, integer(perf0, 'instret_avg'), samples1, integer(perf1, 'instret_avg')):.3f}"
    )

    decode_samples0 = integer(decode0, "samples")
    decode_samples1 = integer(decode1, "samples")
    if decode_samples1 > decode_samples0:
        decode_cycles0 = split_integers(decode0, "cycles")
        decode_cycles1 = split_integers(decode1, "cycles")
        decode_us0 = split_integers(decode0, "dec_us")
        decode_us1 = split_integers(decode1, "dec_us")
        print(f"decode_samples={decode_samples1 - decode_samples0}")
        print(
            "decode_us_avg="
            f"{interval_average(decode_samples0, decode_us0[1], decode_samples1, decode_us1[1]):.3f}"
        )
        print(
            "decode_cycles_avg="
            f"{interval_average(decode_samples0, decode_cycles0[1], decode_samples1, decode_cycles1[1]):.3f}"
        )
        print(
            "decode_instret_avg="
            f"{interval_average(decode_samples0, integer(decode0, 'instret_avg'), decode_samples1, integer(decode1, 'instret_avg')):.3f}"
        )
        print(
            f"decode_bench_frames={integer(decode1, 'bench_frames') - integer(decode0, 'bench_frames')} "
            f"decode_bench_errors={integer(decode1, 'bench_errors') - integer(decode0, 'bench_errors')}"
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

    if decode_samples1 > decode_samples0:
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
