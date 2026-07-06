#!/usr/bin/env python3
"""Print a manifest for built ESP32 and M61 firmware artifacts."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import sys
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class ArtifactSpec:
    name: str
    path: Path
    target: str
    offset: str | None = None


@dataclass(frozen=True)
class ArtifactEntry:
    name: str
    target: str
    path: str
    exists: bool
    size: int | None
    sha256: str | None
    offset: str | None


ARTIFACTS = [
    ArtifactSpec(
        name="esp32-hidh-bootloader",
        target="esp32-hidh",
        offset="0x1000",
        path=ROOT / "build" / "bootloader" / "bootloader.bin",
    ),
    ArtifactSpec(
        name="esp32-hidh-partition-table",
        target="esp32-hidh",
        offset="0x8000",
        path=ROOT / "build" / "partition_table" / "partition-table.bin",
    ),
    ArtifactSpec(
        name="esp32-hidh-app",
        target="esp32-hidh",
        offset="0x10000",
        path=ROOT / "build" / "ds5_dualsense_bridge_esp32.bin",
    ),
    ArtifactSpec(
        name="esp32-raw-hidp-bootloader",
        target="esp32-raw-hidp",
        offset="0x1000",
        path=ROOT / "build_raw_hidp" / "bootloader" / "bootloader.bin",
    ),
    ArtifactSpec(
        name="esp32-raw-hidp-partition-table",
        target="esp32-raw-hidp",
        offset="0x8000",
        path=ROOT / "build_raw_hidp" / "partition_table" / "partition-table.bin",
    ),
    ArtifactSpec(
        name="esp32-raw-hidp-app",
        target="esp32-raw-hidp",
        offset="0x10000",
        path=ROOT / "build_raw_hidp" / "ds5_dualsense_bridge_esp32.bin",
    ),
    ArtifactSpec(
        name="esp32-dual-chip-bootloader",
        target="esp32-dual-chip",
        offset="0x1000",
        path=ROOT / "build_dual_chip" / "bootloader" / "bootloader.bin",
    ),
    ArtifactSpec(
        name="esp32-dual-chip-partition-table",
        target="esp32-dual-chip",
        offset="0x8000",
        path=ROOT / "build_dual_chip" / "partition_table" / "partition-table.bin",
    ),
    ArtifactSpec(
        name="esp32-dual-chip-app",
        target="esp32-dual-chip",
        offset="0x10000",
        path=ROOT / "build_dual_chip" / "ds5_dualsense_bridge_esp32.bin",
    ),
    ArtifactSpec(
        name="esp32-dual-chip-left-bootloader",
        target="esp32-dual-chip-left",
        offset="0x1000",
        path=ROOT / "build_dual_chip_devkit_left" / "bootloader" / "bootloader.bin",
    ),
    ArtifactSpec(
        name="esp32-dual-chip-left-partition-table",
        target="esp32-dual-chip-left",
        offset="0x8000",
        path=ROOT / "build_dual_chip_devkit_left" / "partition_table" / "partition-table.bin",
    ),
    ArtifactSpec(
        name="esp32-dual-chip-left-app",
        target="esp32-dual-chip-left",
        offset="0x10000",
        path=ROOT / "build_dual_chip_devkit_left" / "ds5_dualsense_bridge_esp32.bin",
    ),
    ArtifactSpec(
        name="m61-esp32-bridge",
        target="m61-bridge",
        path=ROOT / "m61" / "esp32_prog_bridge" / "build" / "build_out" /
        "m61_esp32_prog_bridge_bl616.bin",
    ),
    ArtifactSpec(
        name="m61-dualsense-hidp-probe",
        target="m61-hidp-probe",
        path=ROOT / "m61" / "dualsense_hidp_probe" / "build" / "build_out" /
        "m61_dualsense_hidp_probe_bl616.bin",
    ),
    ArtifactSpec(
        name="m61-dualsense-dual-chip-left",
        target="m61-dual-chip-left",
        path=ROOT / "m61" / "dualsense_hidp_probe" / "build_dual_chip_left_spi" /
        "build_out" / "m61_dualsense_hidp_probe_bl616.bin",
    ),
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_manifest(specs: Iterable[ArtifactSpec]) -> list[ArtifactEntry]:
    entries: list[ArtifactEntry] = []
    for spec in specs:
        exists = spec.path.is_file()
        entries.append(
            ArtifactEntry(
                name=spec.name,
                target=spec.target,
                path=str(spec.path.relative_to(ROOT)),
                exists=exists,
                size=spec.path.stat().st_size if exists else None,
                sha256=sha256_file(spec.path) if exists else None,
                offset=spec.offset,
            )
        )
    return entries


def print_text(entries: list[ArtifactEntry]) -> None:
    for entry in entries:
        if not entry.exists:
            print(f"{entry.name}: missing {entry.path}")
            continue
        offset = f" offset={entry.offset}" if entry.offset is not None else ""
        print(
            f"{entry.name}: target={entry.target}{offset} "
            f"size={entry.size} sha256={entry.sha256} path={entry.path}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    parser.add_argument("--strict", action="store_true", help="return failure if any artifact is missing")
    parser.add_argument("-o", "--output", type=Path, help="write manifest to a file")
    args = parser.parse_args(argv)

    entries = build_manifest(ARTIFACTS)
    missing = [entry.path for entry in entries if not entry.exists]

    if args.json:
        text = json.dumps([asdict(entry) for entry in entries], indent=2)
    else:
        lines: list[str] = []
        for entry in entries:
            if not entry.exists:
                lines.append(f"{entry.name}: missing {entry.path}")
            else:
                offset = f" offset={entry.offset}" if entry.offset is not None else ""
                lines.append(
                    f"{entry.name}: target={entry.target}{offset} "
                    f"size={entry.size} sha256={entry.sha256} path={entry.path}"
                )
        text = "\n".join(lines)

    if args.output is not None:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)

    if args.strict and missing:
        print("Missing firmware artifacts:", file=sys.stderr)
        for path in missing:
            print(f"  - {path}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
