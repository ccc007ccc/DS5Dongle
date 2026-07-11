#!/usr/bin/env python3
"""Validate that the final dual-chip artifacts are fresh and release-gated."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from build_esp32_stage1 import DEFAULT_IDF_PATH, DEFAULT_PY_ENV


ROOT = Path(__file__).resolve().parents[1]
ESP_BUILD = ROOT / "build_dual_chip_devkit_left"
M61_BUILD = (
    ROOT
    / "m61"
    / "dualsense_hidp_probe"
    / "build_dual_chip_left_spi"
    / "build_out"
)

ESP_ARTIFACTS = (
    ESP_BUILD / "ds5_dualsense_bridge_esp32.bin",
    ESP_BUILD / "ds5_dualsense_bridge_esp32.elf",
    ESP_BUILD / "ds5_dualsense_bridge_esp32.map",
)
ESP_CONFIG_ARTIFACTS = (ESP_BUILD / "config" / "sdkconfig.h",)
M61_ARTIFACTS = (
    M61_BUILD / "m61_dualsense_hidp_probe_bl616.bin",
    M61_BUILD / "m61_dualsense_hidp_probe_bl616.elf",
)


def _files_under(path: Path, suffixes: set[str]) -> list[Path]:
    return [
        item
        for item in path.rglob("*")
        if item.is_file()
        and item.suffix.lower() in suffixes
        and not any(
            part.startswith("build")
            for part in item.relative_to(path).parts[:-1]
        )
    ]


def esp_inputs() -> list[Path]:
    files = _files_under(ROOT / "main", {".c", ".h", ".txt"})
    files.extend(
        path
        for path in (
            ROOT / "CMakeLists.txt",
            ROOT / "sdkconfig.defaults",
            ROOT / "sdkconfig.dual_chip.defaults",
            ROOT / "sdkconfig.dual_chip.devkit_left.defaults",
            ROOT / "main" / "Kconfig.projbuild",
        )
        if path.is_file()
    )
    return files


def esp_config_inputs() -> list[Path]:
    return [
        path
        for path in (
            ROOT / "sdkconfig.defaults",
            ROOT / "sdkconfig.dual_chip.defaults",
            ROOT / "sdkconfig.dual_chip.devkit_left.defaults",
            ROOT / "main" / "Kconfig.projbuild",
        )
        if path.is_file()
    ]


def m61_inputs() -> list[Path]:
    project = ROOT / "m61" / "dualsense_hidp_probe"
    files = _files_under(project, {".c", ".h", ".txt", ".sh"})
    files.extend(path for path in project.glob("defconfig*") if path.is_file())
    for name in (
        "dual_chip_scheduler_types.c",
        "dual_chip_scheduler_types.h",
        "dual_chip_spi_proto.c",
        "dual_chip_spi_proto.h",
        "dualsense_output.c",
        "dualsense_output.h",
        "dualsense_parser.c",
        "dualsense_parser.h",
    ):
        path = ROOT / "main" / name
        if path.is_file():
            files.append(path)
    return files


def check_freshness(label: str, artifacts: tuple[Path, ...], inputs: list[Path]) -> list[str]:
    failures: list[str] = []
    missing = [path for path in artifacts if not path.is_file()]
    if missing:
        return [f"{label} artifact missing: {path.relative_to(ROOT)}" for path in missing]
    if not inputs:
        return [f"{label} has no configured source inputs"]

    newest_input = max(inputs, key=lambda path: path.stat().st_mtime_ns)
    newest_time = newest_input.stat().st_mtime_ns
    for artifact in artifacts:
        if artifact.stat().st_mtime_ns < newest_time:
            failures.append(
                f"{label} artifact is stale: {artifact.relative_to(ROOT)} is older than "
                f"{newest_input.relative_to(ROOT)}"
            )
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=DEFAULT_IDF_PATH)
    parser.add_argument(
        "--idf-python",
        type=Path,
        default=DEFAULT_PY_ENV / "Scripts" / "python.exe",
    )
    args = parser.parse_args(argv)

    failures = check_freshness("ESP32", ESP_ARTIFACTS, esp_inputs())
    failures.extend(
        check_freshness("ESP32 config", ESP_CONFIG_ARTIFACTS, esp_config_inputs())
    )
    failures.extend(check_freshness("M61", M61_ARTIFACTS, m61_inputs()))
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    if not args.idf_python.is_file():
        print(f"FAIL: ESP-IDF Python not found: {args.idf_python}")
        return 2

    command = [
        str(args.idf_python),
        str(ROOT / "tools" / "check_scheduler_memory.py"),
        "--m61-elf",
        str(M61_ARTIFACTS[1]),
        "--esp-map",
        str(ESP_ARTIFACTS[2]),
        "--idf-path",
        str(args.idf_path),
        "--idf-python",
        str(args.idf_python),
        "--phase",
        "final",
        "--phase-config",
        str(ROOT / "tools" / "scheduler_symbol_phases.json"),
    ]
    print("running:", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        return result.returncode
    print("Dual-chip release artifacts are fresh and gated.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
