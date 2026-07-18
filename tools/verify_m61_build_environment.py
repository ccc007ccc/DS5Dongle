#!/usr/bin/env python3
"""Verify every dependency that affects the validated M61 release build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = ROOT / "m61" / "dualsense_hidp_probe" / "reproducible-build.lock.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text_lf(path: Path) -> str:
    """Hash tracked text canonically so checkout line endings do not matter."""
    data = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def run(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        args,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed ({' '.join(args)}): {detail}")
    return result.stdout.strip()


def git_head(repository: Path) -> str:
    return run("git", "rev-parse", "HEAD", cwd=repository)


def git_status(repository: Path) -> str:
    return run("git", "status", "--porcelain", "--untracked-files=normal", cwd=repository)


def toolchain_platform() -> str:
    return "windows" if os.name == "nt" else "linux"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path, required=True)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--allow-dirty-sdk", action="store_true")
    parser.add_argument("--json", action="store_true", dest="as_json")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    lock_path = args.lock.resolve()
    sdk = args.sdk.resolve()
    toolchain_bin = args.toolchain_bin.resolve()
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    errors: list[str] = []

    if lock.get("schema") != 1:
        errors.append(f"unsupported lock schema: {lock.get('schema')!r}")

    expected_sdk = lock["bouffaloSdk"]
    try:
        actual_sdk_commit = git_head(sdk)
        if actual_sdk_commit != expected_sdk["commit"]:
            errors.append(
                f"Bouffalo SDK commit is {actual_sdk_commit}; expected {expected_sdk['commit']}"
            )
        sdk_changes = git_status(sdk)
        if sdk_changes and not args.allow_dirty_sdk:
            errors.append("Bouffalo SDK worktree is not clean:\n" + sdk_changes)
    except (OSError, RuntimeError) as cause:
        actual_sdk_commit = "unknown"
        errors.append(str(cause))

    platform_name = toolchain_platform()
    expected_toolchain = lock["toolchains"][platform_name]
    gcc_name = "riscv64-unknown-elf-gcc.exe" if platform_name == "windows" else "riscv64-unknown-elf-gcc"
    gcc = toolchain_bin / gcc_name
    actual_toolchain_commit = "unknown"
    gcc_version = "unknown"
    if not gcc.is_file():
        errors.append(f"compiler not found: {gcc}")
    else:
        try:
            gcc_version = run(str(gcc), "--version").splitlines()[0]
            if expected_toolchain["versionContains"] not in gcc_version:
                errors.append(
                    f"compiler version is {gcc_version!r}; expected text "
                    f"{expected_toolchain['versionContains']!r}"
                )
            expected_hash = expected_toolchain.get("gccSha256")
            if expected_hash and sha256(gcc) != expected_hash:
                errors.append(f"compiler SHA256 mismatch: {gcc}")
            toolchain_root = toolchain_bin.parent
            if (toolchain_root / ".git").exists():
                actual_toolchain_commit = git_head(toolchain_root)
                if actual_toolchain_commit != expected_toolchain["commit"]:
                    errors.append(
                        f"toolchain commit is {actual_toolchain_commit}; "
                        f"expected {expected_toolchain['commit']}"
                    )
                changes = git_status(toolchain_root)
                if changes:
                    errors.append("toolchain worktree is not clean:\n" + changes)
        except (OSError, RuntimeError) as cause:
            errors.append(str(cause))

    patch_results: list[dict[str, str]] = []
    for entry in lock["opus"]["patches"]:
        path = ROOT / entry["path"]
        if not path.is_file():
            errors.append(f"Opus patch not found: {entry['path']}")
            continue
        actual_hash = sha256_text_lf(path)
        patch_results.append({"path": entry["path"], "sha256": actual_hash})
        if actual_hash != entry["sha256"]:
            errors.append(
                f"Opus patch SHA256 mismatch for {entry['path']}: "
                f"{actual_hash} != {entry['sha256']}"
            )

    report = {
        "ok": not errors,
        "lock": str(lock_path),
        "sdk": {"path": str(sdk), "commit": actual_sdk_commit},
        "toolchain": {
            "platform": platform_name,
            "bin": str(toolchain_bin),
            "commit": actual_toolchain_commit,
            "version": gcc_version,
        },
        "opusPatches": patch_results,
        "releaseProfile": lock["releaseProfile"],
        "errors": errors,
    }

    if args.as_json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(f"[m61-repro] SDK {actual_sdk_commit}")
        print(f"[m61-repro] Toolchain {gcc_version}")
        print(f"[m61-repro] Opus patch stack: {len(patch_results)} verified")
        for error in errors:
            print(f"[m61-repro] ERROR: {error}", file=sys.stderr)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
