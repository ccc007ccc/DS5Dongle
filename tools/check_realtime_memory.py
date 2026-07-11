#!/usr/bin/env python3
"""Fail closed when the single-chip realtime design exceeds its RAM budget."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


RAM_BYTES = 512 * 1024
STATIC_LIMIT = RAM_BYTES * 80 // 100
STATIC_CONTINGENCY = 8 * 1024
MIN_HEAP_AFTER_RESERVES = 100 * 1024
RUNTIME_CONTINGENCY = 2 * 1024
TYPED_QUEUE_PAYLOAD = (
    1 * 4172  # latest raw audio epoch
    + 4 * 276  # completed audio epochs
    + 2 * 71  # mic Opus ingress
    + 2 * 962  # decoded mic PCM
)


def section_size(text: str, name: str) -> int:
    match = re.search(rf"^{re.escape(name)}\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)",
                      text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing {name} section")
    return int(match.group(1), 16)


def symbol_value(text: str, name: str) -> int:
    match = re.search(rf"^\s*0x([0-9a-fA-F]+)\s+{re.escape(name)}\s*=",
                      text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing {name} symbol")
    return int(match.group(1), 16)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path)
    args = parser.parse_args()

    text = args.map.read_text(encoding="utf-8", errors="replace")
    data = section_size(text, ".data")
    bss = section_size(text, ".bss")
    heap_start = symbol_value(text, "__end__")
    heap_end = symbol_value(text, "__HeapLimit")
    static_used = data + bss
    heap_capacity = heap_end - heap_start
    heap_after_reserves = heap_capacity - TYPED_QUEUE_PAYLOAD - RUNTIME_CONTINGENCY

    print(f"RP2350 static RAM:              {static_used:6d} B")
    print(f"static + 8 KiB contingency:    {static_used + STATIC_CONTINGENCY:6d} B")
    print(f"80% static limit:              {STATIC_LIMIT:6d} B")
    print(f"linker heap capacity:          {heap_capacity:6d} B")
    print(f"typed queue payload:           {TYPED_QUEUE_PAYLOAD:6d} B")
    print(f"heap after queue + contingency:{heap_after_reserves:6d} B")

    failures: list[str] = []
    if static_used + STATIC_CONTINGENCY >= STATIC_LIMIT:
        failures.append("static RAM plus contingency exceeds 80%")
    if heap_after_reserves < MIN_HEAP_AFTER_RESERVES:
        failures.append("heap after typed queues and contingency is below 100 KiB")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("Single-chip realtime memory gate passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(2)
