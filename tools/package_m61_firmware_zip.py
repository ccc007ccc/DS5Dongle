#!/usr/bin/env python3
"""Build one validated, deterministic M61 firmware Release ZIP."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import zipfile


PARTITION_NAME = "partition.bin"
FIRMWARE_NAME = "m61_dualsense_hidp_probe_bl616.bin"
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_manifest(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        digest = fields[0].lower()
        name = Path(fields[1].lstrip("*")).name
        if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
            raise ValueError(f"invalid SHA256 in {path}: {fields[0]}")
        if name in entries:
            raise ValueError(f"duplicate manifest entry: {name}")
        entries[name] = digest
    return entries


def discover_release_files(input_dir: Path, tag: str) -> list[Path]:
    boot2 = sorted(input_dir.glob("boot2_bl616_*.bin"))
    if len(boot2) != 1:
        raise ValueError(f"expected exactly one boot2_bl616_*.bin, found {len(boot2)}")
    partition = input_dir / PARTITION_NAME
    firmware = input_dir / FIRMWARE_NAME
    preferred_manifest = input_dir / f"{tag}-flash-files.sha256"
    manifests = sorted(input_dir.glob("*-flash-files.sha256"))
    manifest = preferred_manifest if preferred_manifest.is_file() else None
    if manifest is None:
        if len(manifests) != 1:
            raise ValueError(f"expected exactly one *-flash-files.sha256, found {len(manifests)}")
        manifest = manifests[0]
    for path in (partition, firmware, manifest):
        if not path.is_file():
            raise ValueError(f"missing Release file: {path}")

    files = [boot2[0], partition, firmware]
    expected = parse_manifest(manifest)
    for path in files:
        expected_hash = expected.get(path.name)
        if expected_hash is None:
            raise ValueError(f"checksum manifest is missing {path.name}")
        actual_hash = sha256(path)
        if actual_hash != expected_hash:
            raise ValueError(
                f"checksum mismatch for {path.name}: expected {expected_hash}, got {actual_hash}"
            )
    return [*files, manifest]


def package_firmware(input_dir: Path, tag: str, output: Path) -> str:
    files = discover_release_files(input_dir, tag)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(
            temporary,
            "w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
            strict_timestamps=True,
        ) as archive:
            for source in files:
                info = zipfile.ZipInfo(source.name, ZIP_TIMESTAMP)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, source.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return sha256(output)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    digest = package_firmware(args.input_dir.resolve(), args.tag, args.output.resolve())
    print(f"{digest}  {args.output.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

