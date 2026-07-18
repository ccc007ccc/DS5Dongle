#!/usr/bin/env python3
"""Repository-level open-source and release-configuration checks."""

from __future__ import annotations

import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


def main() -> int:
    failures: list[str] = []
    required_files = [
        "README.md",
        "README.zh-CN.md",
        ".github/workflows/windows-flasher.yml",
        "CONTRIBUTING.md",
        "CONTRIBUTING.zh-CN.md",
        "THIRD_PARTY_NOTICES.md",
        "benchmarks/PERFORMANCE_BEST.csv",
        "benchmarks/README.md",
        "benchmarks/README.zh-CN.md",
        "docs/ARCHITECTURE.md",
        "docs/ARCHITECTURE.zh-CN.md",
        "docs/BUILDING.md",
        "docs/BUILDING.zh-CN.md",
        "docs/DEVELOPMENT.md",
        "docs/DEVELOPMENT.zh-CN.md",
        "docs/FEATURES.md",
        "docs/FEATURES.zh-CN.md",
        "docs/HARDWARE.md",
        "docs/HARDWARE.zh-CN.md",
        "docs/OPEN_SOURCE.md",
        "docs/OPEN_SOURCE.zh-CN.md",
        "docs/PERFORMANCE.md",
        "docs/PERFORMANCE.zh-CN.md",
        "docs/PROTOCOL.md",
        "docs/PROTOCOL.zh-CN.md",
        "m61/dualsense_hidp_probe/reproducible-build.lock.json",
        "m61/dualsense_hidp_probe/prepare_opus_source.ps1",
        "tools/verify_m61_build_environment.py",
        "tools/generate_m61_build_manifest.py",
        "tools/package_m61_firmware_zip.py",
        "tools/test_package_m61_firmware_zip.py",
        "tools/m61-flasher/Cargo.toml",
        "tools/m61-flasher/Cargo.lock",
        "tools/m61-flasher/README.md",
        "tools/m61-flasher/README.zh-CN.md",
    ]
    forbidden_paths = [
        ".gitmodules",
        "pico_sdk_import.cmake",
        "boards",
        "cmake",
        "lib",
        "src",
        "docs/PROJECT_STANDARD.md",
        "docs/IMPLEMENTATION_STATUS.md",
        "docs/WAKEUP_RUNBOOK.md",
        "tools/audit_requirements.py",
        "tools/test_requirements_audit.py",
        "m61/usb_hid_gamepad_probe",
        "m61/usb_ram_disk_probe",
    ]

    for path in required_files:
        if not (ROOT / path).is_file():
            failures.append(f"missing required file: {path}")
    for path in forbidden_paths:
        if (ROOT / path).exists():
            failures.append(f"obsolete or non-M61 path must not exist: {path}")

    markdown_files = [
        *ROOT.glob("*.md"),
        *(ROOT / "docs").glob("*.md"),
        *(ROOT / "benchmarks").glob("*.md"),
        *(ROOT / "m61" / "dualsense_hidp_probe").glob("*.md"),
        *(ROOT / "tools" / "m61-flasher").glob("*.md"),
    ]
    bilingual_files = [
        ROOT / "README.md",
        ROOT / "CONTRIBUTING.md",
        *(ROOT / "docs").glob("*.md"),
        *(ROOT / "benchmarks").glob("*.md"),
        ROOT / "m61" / "dualsense_hidp_probe" / "README.md",
        ROOT / "tools" / "m61-flasher" / "README.md",
    ]
    for english in bilingual_files:
        if english.name.endswith(".zh-CN.md"):
            continue
        chinese = english.with_name(f"{english.stem}.zh-CN.md")
        if not chinese.is_file():
            failures.append(
                f"missing Simplified Chinese document for {english.relative_to(ROOT)}"
            )

    for markdown in markdown_files:
        content = markdown.read_text(encoding="utf-8")
        for match in MARKDOWN_LINK.finditer(content):
            target = match.group(1).strip().strip("<>").split("#", 1)[0]
            if not target or target.startswith(("http://", "https://", "mailto:")):
                continue
            target = target.split(" ", 1)[0]
            if not (markdown.parent / target).resolve().exists():
                failures.append(
                    f"broken Markdown link in {markdown.relative_to(ROOT)}: {match.group(1)}"
                )

    lock_path = ROOT / "m61/dualsense_hidp_probe/reproducible-build.lock.json"
    if lock_path.is_file():
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        release = lock.get("releaseProfile", {})
        expected = {
            "wramLengthBytes": 163840,
            "opusVariant": "O2-LTO-e907-d4-fastpath",
            "opusTcmProfile": "pvq-mdct-decode-mdct",
            "usbGamepadO2": True,
            "codecPairDelayMs": 1,
            "crc32NibbleTable": True,
            "hpmProfile": False,
            "runtimeProfile": False,
            "pipelineProfile": False,
            "micDefaultEnabled": False,
            "compileTimeCpuOverclockMhz": 0,
            "runtimeCpuMhz": 320,
        }
        for key, value in expected.items():
            if release.get(key) != value:
                failures.append(
                    f"release lock {key}={release.get(key)!r}; expected {value!r}"
                )
        if len(lock.get("opus", {}).get("patches", [])) != 11:
            failures.append("release lock must contain the 11-patch Opus stack")

    cmake_path = ROOT / "m61/dualsense_hidp_probe/CMakeLists.txt"
    opus_cmake_path = (
        ROOT / "m61/dualsense_hidp_probe/cmake/opus-1.2.1-windows/CMakeLists.txt"
    )
    if cmake_path.is_file():
        cmake = cmake_path.read_text(encoding="utf-8")
        for marker in (
            "set(CONFIG_WRAM_LENGTH 163840)",
            "CONFIG_M61_CRC32_NIBBLE_TABLE",
            "CONFIG_M61_USB_GAMEPAD_O2",
        ):
            if marker not in cmake:
                failures.append(f"firmware CMake missing release marker: {marker}")
    if opus_cmake_path.is_file():
        opus_cmake = opus_cmake_path.read_text(encoding="utf-8")
        for marker in (
            "M61_OPUS_E907_FFT_COMPLEX_MAC=1",
            "M61_OPUS_TCM_DECODE_MDCT=1",
            "-flto",
        ):
            if marker not in opus_cmake:
                failures.append(f"Opus CMake missing release marker: {marker}")

    release_artifact_markers = {
        "tools/generate_m61_build_manifest.py": (
            'parser.add_argument("--boot2"',
            'parser.add_argument("--partition"',
            '"boot2":',
            '"partition":',
        ),
        "m61/dualsense_hidp_probe/build_windows.ps1": (
            "'--boot2'",
            "'--partition'",
        ),
        "m61/dualsense_hidp_probe/build.sh": (
            '--boot2 "$boot2_file"',
            '--partition "$partition_file"',
        ),
        ".github/workflows/firmware-ci.yml": (
            "build_out/boot2_bl616_*.bin",
            "build_out/partition.bin",
        ),
        ".github/workflows/windows-flasher.yml": (
            "tools/package_m61_firmware_zip.py",
            "M61-Firmware-${RELEASE_TAG}.zip",
            "--tool-preflight",
        ),
        "tools/m61-flasher/src/main.rs": (
            'asset.name.starts_with("M61-Firmware-")',
            "read_firmware_zip",
            "chips/bl616/eflash_loader/eflash_loader_cfg.ini",
        ),
    }
    for relative_path, markers in release_artifact_markers.items():
        path = ROOT / relative_path
        if not path.is_file():
            continue
        content = path.read_text(encoding="utf-8")
        for marker in markers:
            if marker not in content:
                failures.append(
                    f"complete release artifact flow missing {marker!r} in {relative_path}"
                )

    for pdf in ROOT.rglob("*.pdf"):
        if ".git" not in pdf.parts and "vendor" not in pdf.parts:
            failures.append(f"vendor PDF must not be in the public tree: {pdf.relative_to(ROOT)}")
    for extension in ("*.bin", "*.elf", "*.a"):
        for artifact in ROOT.rglob(extension):
            if not any(
                part in {"build", "build-win", "target", ".cache", "artifacts"}
                for part in artifact.parts
            ):
                failures.append(
                    f"generated binary outside ignored build paths: {artifact.relative_to(ROOT)}"
                )

    if failures:
        print("Project verification failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Project verification passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
