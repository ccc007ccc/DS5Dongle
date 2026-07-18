#!/usr/bin/env python3
"""Build and run the M61 idle-activity boundary tests through WSL."""

from pathlib import Path

from host_c_test import run_host_c_test


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    app = ROOT / "m61" / "dualsense_hidp_probe"
    result = run_host_c_test(
        "m61-dualsense-activity-test",
        [ROOT / "tools" / "test_dualsense_activity.c", app / "dualsense_parser.c"],
        [app],
    )
    if result == 0:
        print("M61 DualSense idle-activity tests passed")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
