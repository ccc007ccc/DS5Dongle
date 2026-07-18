#!/usr/bin/env python3
"""Exercise release-flash artifact preflight without connected hardware."""

from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory

from flash_m61_firmware import flash_artifact_errors


def touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.touch()


def main() -> int:
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "build-win" / "build_out"

        errors = flash_artifact_errors(root, "application_bl616.bin", "build-win", "bl616")
        assert len(errors) == 3
        assert any("firmware" in error for error in errors)
        assert any("partition" in error for error in errors)
        assert any("boot2" in error for error in errors)

        touch(output / "application_bl616.bin")
        touch(output / "partition.bin")
        touch(output / "boot2_bl616_release.bin")
        assert flash_artifact_errors(root, "application_bl616.bin", "build-win", "bl616") == []

        touch(output / "boot2_bl616_second.bin")
        errors = flash_artifact_errors(root, "application_bl616.bin", "build-win", "bl616")
        assert len(errors) == 1 and "multiple boot2" in errors[0]

        (output / "boot2_bl616_second.bin").unlink()
        (output / "partition.bin").unlink()
        errors = flash_artifact_errors(root, "application_bl616.bin", "build-win", "bl616")
        assert len(errors) == 1 and "partition" in errors[0]

    print("M61 flash artifact preflight tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
