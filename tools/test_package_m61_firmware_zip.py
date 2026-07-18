#!/usr/bin/env python3
"""Test deterministic M61 firmware ZIP packaging and checksum gates."""

from __future__ import annotations

import hashlib
from pathlib import Path
from tempfile import TemporaryDirectory
import zipfile

from package_m61_firmware_zip import FIRMWARE_NAME, PARTITION_NAME, package_firmware


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "source"
        source.mkdir()
        boot_name = "boot2_bl616_isp_release_test.bin"
        files = {
            boot_name: bytes([0x42]) * 52_576,
            PARTITION_NAME: bytes([0x50]) * 308,
            FIRMWARE_NAME: bytes([0x61]) * 65_536,
        }
        for name, data in files.items():
            (source / name).write_bytes(data)
        manifest = "".join(f"{digest(data)}  {name}\n" for name, data in files.items())
        manifest_path = source / "v-test-flash-files.sha256"
        manifest_path.write_text(manifest, encoding="utf-8")

        first = root / "first.zip"
        second = root / "second.zip"
        first_hash = package_firmware(source, "v-test", first)
        second_hash = package_firmware(source, "v-test", second)
        assert first_hash == second_hash
        assert first.read_bytes() == second.read_bytes()
        with zipfile.ZipFile(first) as archive:
            assert archive.namelist() == [
                boot_name,
                PARTITION_NAME,
                FIRMWARE_NAME,
                manifest_path.name,
            ]
            for name, data in files.items():
                assert archive.read(name) == data

        manifest_path.write_text(manifest.replace(digest(files[boot_name]), "0" * 64), encoding="utf-8")
        try:
            package_firmware(source, "v-test", root / "bad.zip")
        except ValueError as error:
            assert "checksum mismatch" in str(error)
        else:
            raise AssertionError("bad checksum manifest was accepted")

    print("M61 firmware ZIP packaging tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

