#!/usr/bin/env python3
"""Self-test the M61 realtime static RAM gate."""

from __future__ import annotations

import check_m61_realtime_memory as gate


def size_output(
    ram_bss: int,
    nocache: int = 1816,
    skip: int = 1824,
    itcm: int = 40000,
) -> str:
    return f"""firmware.elf  :
section                       size         addr
ram_nocache_data                 0    586941440
ram_nocache_noinit       {nocache}    586941440
.ram_skip_nocache_region {skip}   1660683264
itcm                      {itcm}   1660685088
dtcm                             0   1660790052
ram_data                      2208   1660790056
ram_bss                   {ram_bss}   1660792272
ram_noinit                    4108   1660946244
Total                       999999
"""


def main() -> int:
    sections = gate.parse_size_output(size_output(150000))
    static_bytes, nocache_bytes, prefix_bytes = gate.calculate_static_ram(sections)
    assert nocache_bytes == 1816
    assert prefix_bytes == 1824
    assert static_bytes == 1824 + 40000 + 2208 + 150000 + 4108
    assert static_bytes + gate.CONTINGENCY_BYTES < (
        gate.RAM_CAPACITY_BYTES * gate.RAM_LIMIT_PERCENT // 100
    )

    # If nocache data is larger than the cached alias skip, count it once.
    sections = gate.parse_size_output(size_output(150000, nocache=2048, skip=1824))
    static_bytes, _, prefix_bytes = gate.calculate_static_ram(sections)
    assert prefix_bytes == 2048
    assert static_bytes == 2048 + 40000 + 2208 + 150000 + 4108

    limit = gate.RAM_CAPACITY_BYTES * gate.RAM_LIMIT_PERCENT // 100
    fixed = 1824 + 40000 + 2208 + 4108 + gate.CONTINGENCY_BYTES
    sections = gate.parse_size_output(size_output(limit - fixed))
    static_bytes, _, _ = gate.calculate_static_ram(sections)
    assert static_bytes + gate.CONTINGENCY_BYTES == limit
    assert not (static_bytes + gate.CONTINGENCY_BYTES < limit)

    sections = gate.parse_size_output(size_output(150000, itcm=40961))
    assert not gate.print_report(sections)

    try:
        gate.parse_size_output("ram_bss 10 20\n")
    except gate.MemoryGateError:
        pass
    else:
        raise AssertionError("missing required sections must fail closed")

    print("M61 realtime memory gate self-test passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
