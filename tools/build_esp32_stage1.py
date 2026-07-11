#!/usr/bin/env python3
"""Build the ESP32 stage-1 firmware with the local ESP-IDF install."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IDF_PATH = Path(r"C:\tmp\esp-idf-v5.3.2")
DEFAULT_IDF_TOOLS_PATH = Path(r"C:\tmp\esp-idf-tools-v5.3.2")
DEFAULT_PY_ENV = DEFAULT_IDF_TOOLS_PATH / "python_env" / "idf5.3_py3.13_env"


def require_dir(path: Path, label: str) -> bool:
    if path.is_dir():
        return True
    print(f"missing {label}: {path}", file=sys.stderr)
    return False


def first_parent_with_exe(root: Path, exe_name: str) -> Path | None:
    matches = sorted(root.rglob(exe_name))
    if not matches:
        return None
    return matches[0].parent


def tool_path_entries(idf_tools_path: Path, py_env: Path) -> list[str]:
    entries: list[Path] = [
        py_env / "Scripts",
    ]

    for rel_root, exe_name in [
        ("tools/cmake", "cmake.exe"),
        ("tools/ninja", "ninja.exe"),
        ("tools/ccache", "ccache.exe"),
        ("tools/xtensa-esp-elf", "xtensa-esp32-elf-gcc.exe"),
        ("tools/esp32ulp-elf", "esp32ulp-elf-gcc.exe"),
    ]:
        parent = first_parent_with_exe(idf_tools_path / rel_root, exe_name)
        if parent is not None:
            entries.append(parent)

    return [str(path) for path in entries if path.is_dir()]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=DEFAULT_IDF_PATH)
    parser.add_argument("--tools-path", type=Path, default=DEFAULT_IDF_TOOLS_PATH)
    parser.add_argument("--python-env", type=Path, default=DEFAULT_PY_ENV)
    parser.add_argument("--target", default="esp32")
    parser.add_argument(
        "--backend",
        choices=("hidh", "raw-hidp", "dual-chip"),
        default="hidh",
        help="stage-1 Bluetooth backend to build",
    )
    parser.add_argument(
        "--pin-profile",
        choices=("none", "devkit-left", "devkit-vspi"),
        default="none",
        help="optional dual-chip wiring profile; default keeps SPI GPIOs disabled",
    )
    parser.add_argument("--clean", action="store_true", help="run idf.py clean before build")
    parser.add_argument(
        "--fullclean",
        action="store_true",
        help="run idf.py fullclean before build to regenerate CMake and Kconfig outputs",
    )
    args = parser.parse_args(argv)

    ok = True
    ok &= require_dir(args.idf_path, "ESP-IDF path")
    ok &= require_dir(args.tools_path, "ESP-IDF tools path")
    ok &= require_dir(args.python_env, "ESP-IDF Python environment")
    if not ok:
        return 1

    python = args.python_env / "Scripts" / "python.exe"
    if not python.is_file():
        print(f"missing ESP-IDF Python: {python}", file=sys.stderr)
        return 1

    idf_py = args.idf_path / "tools" / "idf.py"
    if not idf_py.is_file():
        print(f"missing idf.py: {idf_py}", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["IDF_PATH"] = str(args.idf_path)
    env["IDF_TOOLS_PATH"] = str(args.tools_path)
    env["IDF_PYTHON_ENV_PATH"] = str(args.python_env)
    env["IDF_TARGET"] = args.target
    env["PATH"] = os.pathsep.join(tool_path_entries(args.tools_path, args.python_env) + [env.get("PATH", "")])

    idf_global_args: list[str] = []
    if args.pin_profile != "none" and args.backend != "dual-chip":
        print("--pin-profile requires --backend dual-chip", file=sys.stderr)
        return 1

    if args.backend in ("raw-hidp", "dual-chip"):
        backend_defaults = (
            ROOT / "sdkconfig.raw_hidp.defaults"
            if args.backend == "raw-hidp"
            else ROOT / "sdkconfig.dual_chip.defaults"
        )
        if not backend_defaults.is_file():
            print(f"missing {args.backend} defaults: {backend_defaults}", file=sys.stderr)
            return 1
        defaults = ["sdkconfig.defaults", backend_defaults.name]
        build_dir = "build_raw_hidp" if args.backend == "raw-hidp" else "build_dual_chip"
        sdkconfig = "sdkconfig.raw_hidp" if args.backend == "raw-hidp" else "sdkconfig.dual_chip"
        if args.pin_profile == "devkit-left":
            profile_defaults = ROOT / "sdkconfig.dual_chip.devkit_left.defaults"
            if not profile_defaults.is_file():
                print(f"missing pin profile defaults: {profile_defaults}", file=sys.stderr)
                return 1
            defaults.append(profile_defaults.name)
            build_dir = "build_dual_chip_devkit_left"
            sdkconfig = "sdkconfig.dual_chip.devkit_left"
        elif args.pin_profile == "devkit-vspi":
            profile_defaults = ROOT / "sdkconfig.dual_chip.devkit_vspi.defaults"
            if not profile_defaults.is_file():
                print(f"missing pin profile defaults: {profile_defaults}", file=sys.stderr)
                return 1
            defaults.append(profile_defaults.name)
            build_dir = "build_dual_chip_devkit_vspi"
            sdkconfig = "sdkconfig.dual_chip.devkit_vspi"
        idf_global_args.extend([
            "-B", build_dir,
            f"-DSDKCONFIG={sdkconfig}",
            f"-DSDKCONFIG_DEFAULTS={';'.join(defaults)}",
        ])

    if args.clean and args.fullclean:
        print("--clean and --fullclean are mutually exclusive", file=sys.stderr)
        return 1

    if args.clean or args.fullclean:
        clean_action = "fullclean" if args.fullclean else "clean"
        clean_cmd = [str(python), str(idf_py), *idf_global_args, clean_action]
        print("running:", " ".join(clean_cmd), flush=True)
        result = subprocess.run(clean_cmd, cwd=ROOT, env=env)
        if result.returncode != 0:
            return result.returncode

    build_cmd = [str(python), str(idf_py), *idf_global_args, "build"]
    print("running:", " ".join(build_cmd), flush=True)
    return subprocess.run(build_cmd, cwd=ROOT, env=env).returncode


if __name__ == "__main__":
    raise SystemExit(main())
