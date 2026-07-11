#!/usr/bin/env python3
"""Enforce post-link RAM limits for the dual-chip scheduler firmware."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Iterable


M61_STATIC_RESERVE = 8 * 1024
M61_STATIC_LIMIT = 318_720
M61_HEAP_RESERVE = 8 * 1024
M61_HEAP_MIN_AFTER_RESERVES = 100 * 1024
ESP_STATIC_RESERVE = 8 * 1024
ESP_STATIC_PERCENT = 70

M61_LINK_SYMBOLS = ("__ram_start__", "__HeapBase", "__HeapLimit")
TARGETS = ("m61", "esp", "any")


class GateError(RuntimeError):
    """A build artifact cannot be measured safely."""


def _symbols_with_pyelftools(elf_path: Path) -> dict[str, int]:
    from elftools.elf.elffile import ELFFile  # type: ignore[import-not-found]

    symbols: dict[str, int] = {}
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        for section in elf.iter_sections():
            if section.header.sh_type not in ("SHT_SYMTAB", "SHT_DYNSYM"):
                continue
            for symbol in section.iter_symbols():
                if symbol.name:
                    symbols.setdefault(symbol.name, int(symbol.entry.st_value))
    return symbols


def _nm_candidates(explicit_nm: str | None) -> Iterable[str]:
    if explicit_nm:
        yield explicit_nm
        return
    env_nm = os.environ.get("M61_NM")
    if env_nm:
        yield env_nm
    toolchain_bin = os.environ.get("M61_TOOLCHAIN_BIN")
    if toolchain_bin:
        yield str(Path(toolchain_bin) / "riscv64-unknown-elf-nm")
    yield "riscv64-unknown-elf-nm"


def _symbols_with_nm(elf_path: Path, explicit_nm: str | None) -> dict[str, int]:
    attempted: list[str] = []
    for candidate in _nm_candidates(explicit_nm):
        resolved = shutil.which(candidate) or (candidate if Path(candidate).is_file() else None)
        if not resolved:
            attempted.append(candidate)
            continue
        attempted.append(str(resolved))
        result = subprocess.run(
            [str(resolved), "-a", "--defined-only", str(elf_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            continue
        symbols: dict[str, int] = {}
        for line in result.stdout.splitlines():
            match = re.match(r"^\s*([0-9A-Fa-f]+)\s+\S\s+(.+?)\s*$", line)
            if match:
                symbols[match.group(2)] = int(match.group(1), 16)
        if symbols:
            return symbols
    raise GateError(
        "pyelftools is unavailable and no working target nm was found; tried: "
        + ", ".join(attempted)
    )


def read_elf_symbols(elf_path: Path, nm_path: str | None = None) -> dict[str, int]:
    try:
        return _symbols_with_pyelftools(elf_path)
    except ImportError:
        return _symbols_with_nm(elf_path, nm_path)
    except (OSError, ValueError) as exc:
        raise GateError(f"cannot read ELF {elf_path}: {exc}") from exc


def measure_m61(elf_path: Path, nm_path: str | None = None) -> tuple[dict[str, int], dict[str, int]]:
    symbols = read_elf_symbols(elf_path, nm_path)
    missing = [name for name in M61_LINK_SYMBOLS if name not in symbols]
    if missing:
        raise GateError(f"M61 ELF is missing linker symbols: {', '.join(missing)}")
    ram_start = symbols["__ram_start__"]
    heap_base = symbols["__HeapBase"]
    heap_limit = symbols["__HeapLimit"]
    if not ram_start <= heap_base <= heap_limit:
        raise GateError(
            "invalid M61 OCRAM layout: expected __ram_start__ <= __HeapBase <= __HeapLimit"
        )
    static_used = heap_base - ram_start
    heap_capacity = heap_limit - heap_base
    return symbols, {
        "static_used": static_used,
        "static_with_reserve": static_used + M61_STATIC_RESERVE,
        "heap_capacity": heap_capacity,
        "heap_after_reserves": heap_capacity - M61_STATIC_RESERVE - M61_HEAP_RESERVE,
    }


def _infer_idf_path(esp_map: Path) -> Path | None:
    env_path = os.environ.get("IDF_PATH")
    if env_path:
        return Path(env_path)
    description = esp_map.parent / "project_description.json"
    if description.is_file():
        try:
            value = json.loads(description.read_text(encoding="utf-8")).get("idf_path")
            if value:
                return Path(value)
        except (OSError, json.JSONDecodeError):
            pass
    return None


def run_idf_size(
    esp_map: Path,
    idf_path: Path | None = None,
    python_executable: str | None = None,
) -> dict[str, object]:
    idf_path = idf_path or _infer_idf_path(esp_map)
    if idf_path is None:
        raise GateError("IDF_PATH is not set and no project_description.json supplied an idf_path")
    tool = idf_path / "tools" / "idf_size.py"
    if not tool.is_file():
        raise GateError(f"official IDF size tool not found: {tool}")
    result = subprocess.run(
        [python_executable or sys.executable, str(tool), "--format", "json2", str(esp_map)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise GateError(f"idf_size.py failed: {detail}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise GateError(f"idf_size.py returned invalid JSON: {exc}") from exc


def measure_esp(size_report: dict[str, object]) -> dict[str, int]:
    layout = size_report.get("layout")
    if not isinstance(layout, list):
        raise GateError("idf_size.py json2 report has no layout array")
    dram = next(
        (entry for entry in layout if isinstance(entry, dict) and entry.get("name") == "DRAM"),
        None,
    )
    if dram is None:
        raise GateError("idf_size.py json2 report has no DRAM entry")
    try:
        used = int(dram["used"])
        total = int(dram["total"])
    except (KeyError, TypeError, ValueError) as exc:
        raise GateError("invalid DRAM used/total values in idf_size.py report") from exc
    if used < 0 or total <= 0 or used > total:
        raise GateError(f"invalid ESP DRAM measurement: used={used}, total={total}")
    limit = total * ESP_STATIC_PERCENT // 100
    return {
        "used": used,
        "total": total,
        "used_with_reserve": used + ESP_STATIC_RESERVE,
        "limit": limit,
    }


def _empty_rules() -> dict[str, dict[str, list[str]]]:
    return {
        "require": {target: [] for target in TARGETS},
        "forbid": {target: [] for target in TARGETS},
    }


def _add_symbol_rule(rules: dict[str, dict[str, list[str]]], kind: str, value: str) -> None:
    target, separator, symbol = value.partition(":")
    if not separator:
        target, symbol = "any", value
    if target not in TARGETS or not symbol:
        raise GateError(f"invalid phase symbol rule {value!r}; use [m61|esp|any]:SYMBOL")
    rules[kind][target].append(symbol)


def load_phase_rules(
    phase: str,
    config_path: Path | None,
    required: list[str],
    forbidden: list[str],
) -> dict[str, dict[str, list[str]]]:
    rules = _empty_rules()
    if config_path:
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, AttributeError) as exc:
            raise GateError(f"cannot read phase config {config_path}: {exc}") from exc
        if not isinstance(config, dict):
            raise GateError(f"phase config {config_path} must contain an object")
        if phase not in config:
            raise GateError(
                f"phase {phase!r} is not defined in phase config {config_path}"
            )
        phase_config = config[phase]
        if not isinstance(phase_config, dict):
            raise GateError(f"phase {phase!r} rules must be an object")
        for kind in ("require", "forbid"):
            groups = phase_config.get(kind, {})
            if not isinstance(groups, dict):
                raise GateError(f"phase {phase!r} {kind!r} rules must be an object")
            for target, names in groups.items():
                if target not in TARGETS or not isinstance(names, list):
                    raise GateError(f"invalid phase {phase!r} {kind!r} target {target!r}")
                rules[kind][target].extend(str(name) for name in names)
    for value in required:
        _add_symbol_rule(rules, "require", value)
    for value in forbidden:
        _add_symbol_rule(rules, "forbid", value)
    return rules


def _map_has_symbol(map_text: str, symbol: str) -> bool:
    return re.search(rf"(?<![A-Za-z0-9_$]){re.escape(symbol)}(?![A-Za-z0-9_$])", map_text) is not None


def check_phase_symbols(
    rules: dict[str, dict[str, list[str]]],
    m61_symbols: set[str],
    esp_map_text: str,
) -> list[str]:
    def present(target: str, symbol: str) -> bool:
        if target == "m61":
            return symbol in m61_symbols
        if target == "esp":
            return _map_has_symbol(esp_map_text, symbol)
        return symbol in m61_symbols or _map_has_symbol(esp_map_text, symbol)

    failures: list[str] = []
    for target in TARGETS:
        for symbol in rules["require"][target]:
            if not present(target, symbol):
                failures.append(f"phase requires missing {target} symbol: {symbol}")
        for symbol in rules["forbid"][target]:
            if present(target, symbol):
                failures.append(f"phase forbids present {target} symbol: {symbol}")
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--m61-elf", type=Path, help="linked BL616 application ELF")
    parser.add_argument("--esp-map", type=Path, help="linked ESP-IDF application map")
    parser.add_argument("--m61-nm", help="target riscv64-unknown-elf-nm fallback")
    parser.add_argument("--idf-path", type=Path, help="ESP-IDF root; defaults to IDF_PATH")
    parser.add_argument("--idf-python", help="Python interpreter from the activated IDF environment")
    parser.add_argument("--phase", default="current", help="migration phase name (default: current)")
    parser.add_argument("--phase-config", type=Path, help="JSON phase symbol rules")
    parser.add_argument("--require-symbol", action="append", default=[], metavar="[TARGET:]SYMBOL")
    parser.add_argument("--forbid-symbol", action="append", default=[], metavar="[TARGET:]SYMBOL")
    args = parser.parse_args()
    if not args.m61_elf and not args.esp_map:
        parser.error("at least one of --m61-elf or --esp-map is required")
    return args


def main() -> int:
    args = parse_args()
    failures: list[str] = []
    m61_symbols: dict[str, int] = {}
    esp_map_text = ""
    try:
        if args.m61_elf:
            m61_symbols, measured = measure_m61(args.m61_elf, args.m61_nm)
            print("M61 post-link OCRAM:")
            print(f"  static used                       {measured['static_used']:7d} B")
            print(f"  static + 8 KiB reserve            {measured['static_with_reserve']:7d} B")
            print(f"  75% static limit                  {M61_STATIC_LIMIT:7d} B")
            print(f"  initial heap capacity             {measured['heap_capacity']:7d} B")
            print(f"  heap - static/heap reserves       {measured['heap_after_reserves']:7d} B")
            print(f"  heap target after reserves        {M61_HEAP_MIN_AFTER_RESERVES:7d} B")
            if measured["static_with_reserve"] > M61_STATIC_LIMIT:
                failures.append("M61 static RAM plus reserve exceeds 75% limit")
            if measured["heap_after_reserves"] < M61_HEAP_MIN_AFTER_RESERVES:
                failures.append("M61 initial heap after both reserves is below 100 KiB")

        if args.esp_map:
            report = run_idf_size(args.esp_map, args.idf_path, args.idf_python)
            measured = measure_esp(report)
            print("ESP32 post-link DRAM:")
            print(f"  static used                       {measured['used']:7d} B")
            print(f"  static + 8 KiB reserve            {measured['used_with_reserve']:7d} B")
            print(f"  70% static limit                  {measured['limit']:7d} B")
            print(f"  total DRAM                        {measured['total']:7d} B")
            if measured["used_with_reserve"] > measured["limit"]:
                failures.append("ESP32 static DRAM plus reserve exceeds 70% limit")
            esp_map_text = args.esp_map.read_text(encoding="utf-8", errors="replace")

        rules = load_phase_rules(
            args.phase, args.phase_config, args.require_symbol, args.forbid_symbol
        )
        failures.extend(check_phase_symbols(rules, set(m61_symbols), esp_map_text))
        rule_count = sum(len(names) for kind in rules.values() for names in kind.values())
        print(f"Phase symbol contract: {args.phase} ({rule_count} configured rules)")
    except (GateError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 2

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("Post-link scheduler memory gate passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
