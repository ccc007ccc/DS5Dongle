#!/usr/bin/env python3
"""Build and run the pure M61 Web management protocol host tests through WSL."""

from __future__ import annotations

from pathlib import Path

from host_c_test import run_host_c_test


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    project = ROOT / "m61" / "dualsense_hidp_probe"
    test = ROOT / "tools" / "test_m61_web_config.c"
    source = project / "m61_web_config.c"
    return run_host_c_test(
        "m61-web-config-test",
        [test, source],
        [project],
    )


if __name__ == "__main__":
    raise SystemExit(main())
