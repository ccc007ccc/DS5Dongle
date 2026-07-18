#!/usr/bin/env python3
"""Write a machine-readable provenance manifest next to an M61 firmware."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = ROOT / "m61" / "dualsense_hidp_probe" / "reproducible-build.lock.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(repository: Path, *args: str) -> str:
    return subprocess.check_output(
        ("git", *args), cwd=repository, text=True, encoding="utf-8"
    ).strip()


def optional_git_output(repository: Path, *args: str, default: str = "unknown") -> str:
    try:
        return git_output(repository, *args)
    except (OSError, subprocess.CalledProcessError):
        return default


def parse_setting(value: str) -> tuple[str, object]:
    key, separator, raw = value.partition("=")
    if not separator or not key:
        raise argparse.ArgumentTypeError("settings must use name=value")
    if raw in {"true", "false"}:
        parsed: object = raw == "true"
    else:
        try:
            parsed = int(raw)
        except ValueError:
            parsed = raw
    return key, parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--boot2", type=Path, required=True)
    parser.add_argument("--partition", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", dest="map_file", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--source-date-epoch", type=int, required=True)
    parser.add_argument("--setting", action="append", default=[], type=parse_setting)
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    source_status = git_output(ROOT, "status", "--porcelain", "--untracked-files=no")
    compiler = args.toolchain_bin / (
        "riscv64-unknown-elf-gcc.exe" if (args.toolchain_bin / "riscv64-unknown-elf-gcc.exe").exists()
        else "riscv64-unknown-elf-gcc"
    )
    compiler_version = subprocess.check_output(
        (str(compiler), "--version"), text=True, encoding="utf-8", errors="replace"
    ).splitlines()[0]
    settings = dict(args.setting)
    commit_source_date_epoch = int(git_output(ROOT, "show", "-s", "--format=%ct", "HEAD"))

    source_repository = optional_git_output(ROOT, "config", "--get", "remote.fork.url")
    if source_repository == "unknown":
        source_repository = optional_git_output(ROOT, "config", "--get", "remote.origin.url")

    manifest = {
        "schema": 1,
        "profile": (
            "release"
            if settings == lock["releaseProfile"]
            and args.source_date_epoch == commit_source_date_epoch
            else "custom"
        ),
        "source": {
            "repository": source_repository,
            "commit": git_output(ROOT, "rev-parse", "HEAD"),
            "dirty": bool(source_status),
            "sourceDateEpoch": args.source_date_epoch,
        },
        "dependencies": {
            "lockSha256": sha256(args.lock),
            "bouffaloSdkCommit": git_output(args.sdk.resolve(), "rev-parse", "HEAD"),
            "toolchainVersion": compiler_version,
            "toolchainCommit": optional_git_output(
                args.toolchain_bin.resolve().parent, "rev-parse", "HEAD"
            ),
            "opusArchiveSha256": lock["opus"]["archiveSha256"],
            "opusPatchSha256": [entry["sha256"] for entry in lock["opus"]["patches"]],
        },
        "settings": settings,
        "artifacts": {
            "boot2": {"name": args.boot2.name, "sha256": sha256(args.boot2)},
            "partition": {
                "name": args.partition.name,
                "sha256": sha256(args.partition),
            },
            "firmware": {"name": args.firmware.name, "sha256": sha256(args.firmware)},
            "elf": {"name": args.elf.name, "sha256": sha256(args.elf)},
            "map": {"name": args.map_file.name, "sha256": sha256(args.map_file)},
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"[m61-repro] Manifest: {args.output}")
    print(f"[m61-repro] Firmware SHA256: {manifest['artifacts']['firmware']['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
