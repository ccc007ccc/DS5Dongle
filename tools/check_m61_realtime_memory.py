#!/usr/bin/env python3
"""Gate BL616 realtime firmware static RAM usage from GNU size -A output."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


RAM_CAPACITY_BYTES = 0x67C00
RAM_LIMIT_PERCENT = 75
CONTINGENCY_BYTES = 8 * 1024
ITCM_BOOT_LIMIT_BYTES = 40 * 1024

NOCACHE_SECTIONS = ("ram_nocache_data", "ram_nocache_noinit")
NOCACHE_ALIAS_SKIP_SECTION = ".ram_skip_nocache_region"
MAIN_RAM_SECTIONS = ("itcm", "dtcm", "ram_data", "ram_bss", "ram_noinit")
REQUIRED_SECTIONS = ("itcm", "ram_data", "ram_bss", "ram_noinit")


class MemoryGateError(RuntimeError):
    """Raised when size output cannot be obtained or interpreted safely."""


def parse_size_output(text: str) -> dict[str, int]:
    sections: dict[str, int] = {}
    for raw_line in text.splitlines():
        fields = raw_line.split()
        if len(fields) < 3:
            continue
        name = fields[0]
        try:
            size = int(fields[1], 0)
            int(fields[2], 0)
        except ValueError:
            continue
        sections[name] = size

    missing = [name for name in REQUIRED_SECTIONS if name not in sections]
    if missing:
        raise MemoryGateError(
            "GNU size output is missing required BL616 RAM sections: " + ", ".join(missing)
        )
    return sections


def calculate_static_ram(sections: dict[str, int]) -> tuple[int, int, int]:
    nocache_bytes = sum(sections.get(name, 0) for name in NOCACHE_SECTIONS)
    alias_skip_bytes = sections.get(NOCACHE_ALIAS_SKIP_SECTION, 0)

    # ram_nocache and ram are aliases of the same physical BL616 RAM. The linker
    # skip section reserves the cached alias of the nocache prefix, so count the
    # larger representation once instead of summing both views.
    physical_prefix_bytes = max(nocache_bytes, alias_skip_bytes)
    main_bytes = sum(sections.get(name, 0) for name in MAIN_RAM_SECTIONS)
    return physical_prefix_bytes + main_bytes, nocache_bytes, physical_prefix_bytes


def windows_to_wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive, tail = os.path.splitdrive(str(resolved))
    if not drive:
        return str(resolved).replace("\\", "/")
    return f"/mnt/{drive[0].lower()}{tail.replace(chr(92), '/')}"


def size_commands(elf: Path, requested_tool: str | None) -> list[list[str]]:
    commands: list[list[str]] = []
    tool = requested_tool or os.environ.get("M61_SIZE_TOOL")
    if tool:
        if os.name == "nt" and tool.startswith("/"):
            commands.append(["wsl", tool, "-A", windows_to_wsl_path(elf)])
        else:
            commands.append([tool, "-A", str(elf)])
        return commands

    native_tool = shutil.which("riscv64-unknown-elf-size")
    if native_tool:
        commands.append([native_tool, "-A", str(elf)])

    if os.name == "nt" and shutil.which("wsl"):
        wsl_elf = windows_to_wsl_path(elf)
        commands.extend(
            [
                [
                    "wsl",
                    "riscv64-unknown-elf-size",
                    "-A",
                    wsl_elf,
                ],
                [
                    "wsl",
                    "/opt/toolchain_gcc_t-head_linux/bin/riscv64-unknown-elf-size",
                    "-A",
                    wsl_elf,
                ],
            ]
        )
    return commands


def read_size_output(elf: Path, requested_tool: str | None) -> str:
    if not elf.is_file():
        raise MemoryGateError(f"ELF does not exist: {elf}")

    commands = size_commands(elf, requested_tool)
    if not commands:
        raise MemoryGateError(
            "riscv64-unknown-elf-size was not found; pass --size-tool or set M61_SIZE_TOOL"
        )

    failures: list[str] = []
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        failures.append(f"{' '.join(command)}: {detail}")
    raise MemoryGateError("all size commands failed:\n  " + "\n  ".join(failures))


def print_report(sections: dict[str, int]) -> bool:
    static_bytes, nocache_bytes, physical_prefix_bytes = calculate_static_ram(sections)
    budgeted_bytes = static_bytes + CONTINGENCY_BYTES
    limit_bytes = RAM_CAPACITY_BYTES * RAM_LIMIT_PERCENT // 100
    remaining_bytes = limit_bytes - budgeted_bytes

    print("M61 realtime RAM gate:")
    for name in (*NOCACHE_SECTIONS, NOCACHE_ALIAS_SKIP_SECTION, *MAIN_RAM_SECTIONS):
        print(f"  {name:26s} {sections.get(name, 0):8d} B")
    print(f"  {'nocache raw total':26s} {nocache_bytes:8d} B")
    print(f"  {'physical nocache prefix':26s} {physical_prefix_bytes:8d} B")
    print(f"  {'static physical RAM':26s} {static_bytes:8d} B")
    print(f"  {'contingency':26s} {CONTINGENCY_BYTES:8d} B")
    print(f"  {'static + contingency':26s} {budgeted_bytes:8d} B")
    print(f"  {'75% limit':26s} {limit_bytes:8d} B")
    print(f"  {'margin below limit':26s} {remaining_bytes:8d} B")
    print(f"  {'physical RAM capacity':26s} {RAM_CAPACITY_BYTES:8d} B")
    print(f"  {'ITCM boot-safe limit':26s} {ITCM_BOOT_LIMIT_BYTES:8d} B")

    itcm_bytes = sections.get("itcm", 0)
    passed = budgeted_bytes < limit_bytes and itcm_bytes <= ITCM_BOOT_LIMIT_BYTES
    if itcm_bytes > ITCM_BOOT_LIMIT_BYTES:
        print(
            f"M61 ITCM boot gate failed: {itcm_bytes} B exceeds "
            f"{ITCM_BOOT_LIMIT_BYTES} B."
        )
    print("M61 realtime RAM gate passed." if passed else "M61 realtime RAM gate failed.")
    return passed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", nargs="?", type=Path, help="BL616 firmware ELF")
    parser.add_argument("--size-tool", help="riscv64-unknown-elf-size executable")
    parser.add_argument(
        "--size-output",
        type=Path,
        help="parse previously captured GNU size -A output instead of invoking the tool",
    )
    args = parser.parse_args(argv)

    if bool(args.elf) == bool(args.size_output):
        parser.error("provide exactly one ELF or --size-output")

    try:
        if args.size_output:
            output = args.size_output.read_text(encoding="utf-8")
        else:
            output = read_size_output(args.elf, args.size_tool)
        sections = parse_size_output(output)
    except (MemoryGateError, OSError) as exc:
        print(f"M61 realtime RAM gate error: {exc}", file=sys.stderr)
        return 2

    return 0 if print_report(sections) else 1


if __name__ == "__main__":
    raise SystemExit(main())
