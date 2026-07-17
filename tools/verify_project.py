#!/usr/bin/env python3
"""Repository-level open-source and release-configuration checks."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    failures: list[str] = []
    required_files = [
        "README.md",
        "README.zh-CN.md",
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
    ]

    for path in required_files:
        if not (ROOT / path).is_file():
            failures.append(f"missing required file: {path}")
    for path in forbidden_paths:
        if (ROOT / path).exists():
            failures.append(f"obsolete or non-M61 path must not exist: {path}")

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

    for pdf in ROOT.rglob("*.pdf"):
        if ".git" not in pdf.parts and "vendor" not in pdf.parts:
            failures.append(f"vendor PDF must not be in the public tree: {pdf.relative_to(ROOT)}")
    for extension in ("*.bin", "*.elf", "*.a"):
        for artifact in ROOT.rglob(extension):
            if not any(
                part in {"build", "build-win", ".cache", "artifacts"}
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
