#!/usr/bin/env python3
"""Pure-logic and source-contract tests for the M61 inactivity policy."""

from __future__ import annotations

from dataclasses import dataclass, replace
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INACTIVITY_C = ROOT / "m61" / "dualsense_hidp_probe" / "m61_ds5_inactivity.c"
M61_MAIN = ROOT / "m61" / "dualsense_hidp_probe" / "main.c"


@dataclass(frozen=True)
class State:
    full: bool = True
    left_x: int = 128
    left_y: int = 128
    right_x: int = 128
    right_y: int = 128
    l2: int = 0
    r2: int = 0
    dpad: int = 8
    buttons: int = 0


@dataclass
class Tracker:
    last_activity_ms: int = 0
    inactive_minutes: int = 0
    armed: bool = False
    disconnect_requested: bool = False


def neutral(state: State) -> bool:
    return (
        state.full
        and 120 <= state.left_x <= 140
        and 120 <= state.left_y <= 140
        and 120 <= state.right_x <= 140
        and 120 <= state.right_y <= 140
        and state.l2 == 0
        and state.r2 == 0
        and state.dpad == 8
        and state.buttons == 0
    )


def note(tracker: Tracker, state: State, now_ms: int, minutes: int) -> bool:
    if not state.full:
        return False
    if minutes == 0:
        tracker.__dict__.update(Tracker().__dict__)
        return False
    if not tracker.armed or tracker.inactive_minutes != minutes:
        tracker.last_activity_ms = now_ms
        tracker.inactive_minutes = minutes
        tracker.armed = True
        tracker.disconnect_requested = False
        return False
    if not neutral(state):
        tracker.last_activity_ms = now_ms
        tracker.disconnect_requested = False
        return False
    if tracker.disconnect_requested:
        return False
    if ((now_ms - tracker.last_activity_ms) & 0xFFFFFFFF) > minutes * 60_000:
        tracker.last_activity_ms = now_ms
        tracker.disconnect_requested = True
        return True
    return False


def main() -> int:
    idle = State()
    assert neutral(replace(idle, left_x=120))
    assert neutral(replace(idle, right_y=140))
    for active in (
        replace(idle, left_x=119),
        replace(idle, left_y=141),
        replace(idle, right_x=119),
        replace(idle, right_y=141),
        replace(idle, l2=1),
        replace(idle, r2=1),
        replace(idle, dpad=0),
        replace(idle, buttons=1),
        replace(idle, full=False),
    ):
        assert not neutral(active)

    tracker = Tracker()
    assert not note(tracker, idle, 100, 0)
    assert not tracker.armed
    assert not note(tracker, idle, 1_000, 1)
    assert not note(tracker, idle, 61_000, 1), "upstream comparison is strictly greater"
    assert note(tracker, idle, 61_001, 1)
    assert not note(tracker, idle, 200_000, 1), "disconnect request must latch once"

    tracker = Tracker()
    assert not note(tracker, idle, 0, 1)
    assert not note(tracker, replace(idle, buttons=1), 50_000, 1)
    assert not note(tracker, idle, 110_000, 1)
    assert note(tracker, idle, 110_001, 1)

    tracker = Tracker()
    assert not note(tracker, idle, 0, 1)
    assert not note(tracker, idle, 60_001, 2), "config changes re-arm the timer"
    assert not note(tracker, replace(idle, full=False), 200_000, 2)

    inactivity_source = INACTIVITY_C.read_text(encoding="utf-8")
    main_source = M61_MAIN.read_text(encoding="utf-8")
    for snippet in (
        "DS5_INACTIVE_AXIS_MIN 120U",
        "DS5_INACTIVE_AXIS_MAX 140U",
        "state->l2 == 0U",
        "state->r2 == 0U",
        "state->dpad == DS5_INACTIVE_DPAD_NEUTRAL",
        "state->buttons == 0U",
        "> timeout_ms",
    ):
        assert snippet in inactivity_source, f"missing inactivity contract: {snippet}"
    assert main_source.count("note_controller_inactivity(") >= 3
    assert "process_controller_inactivity_disconnect();" in main_source
    assert "m61_esp32_transport_disconnect(true)" in main_source
    assert "bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN)" in main_source

    print("M61 inactivity policy tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
