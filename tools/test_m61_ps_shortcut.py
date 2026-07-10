#!/usr/bin/env python3
"""Pure-logic and source-contract tests for the M61 PS shortcuts."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHORTCUT_C = ROOT / "m61" / "dualsense_hidp_probe" / "m61_ps_shortcut.c"
USB_C = ROOT / "m61" / "dualsense_hidp_probe" / "m61_usb_gamepad.c"
MAIN_C = ROOT / "m61" / "dualsense_hidp_probe" / "main.c"
CMAKE = ROOT / "m61" / "dualsense_hidp_probe" / "CMakeLists.txt"

NONE = 0
WIN_G = 1
WIN_TAB = 2


@dataclass
class Shortcut:
    press_time_ms: int = 0
    last_high_time_ms: int = 0
    pending_action: int = NONE
    stable_pressed: bool = False
    was_pressed: bool = False
    long_press_fired: bool = False


def elapsed(now: int, then: int) -> int:
    return (now - then) & 0xFFFFFFFF


def note(shortcut: Shortcut, raw_pressed: bool, now_ms: int, enabled: bool = True) -> None:
    if not enabled:
        shortcut.__dict__.update(Shortcut().__dict__)
        return
    if raw_pressed:
        shortcut.stable_pressed = True
        shortcut.last_high_time_ms = now_ms
    elif elapsed(now_ms, shortcut.last_high_time_ms) > 50:
        shortcut.stable_pressed = False

    if shortcut.stable_pressed and not shortcut.was_pressed:
        shortcut.press_time_ms = now_ms
        shortcut.was_pressed = True
        shortcut.long_press_fired = False
    elif shortcut.stable_pressed and shortcut.was_pressed:
        if not shortcut.long_press_fired and elapsed(now_ms, shortcut.press_time_ms) >= 750:
            shortcut.pending_action = WIN_TAB
            shortcut.long_press_fired = True
    elif not shortcut.stable_pressed and shortcut.was_pressed:
        if not shortcut.long_press_fired:
            shortcut.pending_action = WIN_G
        shortcut.was_pressed = False


def take(shortcut: Shortcut) -> int:
    action = shortcut.pending_action
    shortcut.pending_action = NONE
    return action


def main() -> int:
    short = Shortcut()
    note(short, True, 100)
    note(short, False, 120)
    note(short, False, 150)
    assert take(short) == NONE, "low level must remain debounced through exactly 50 ms"
    note(short, False, 151)
    assert take(short) == WIN_G

    bounced = Shortcut()
    note(bounced, True, 100)
    note(bounced, False, 140)
    note(bounced, True, 145)
    note(bounced, False, 195)
    assert take(bounced) == NONE
    note(bounced, False, 196)
    assert take(bounced) == WIN_G

    long = Shortcut()
    note(long, True, 1_000)
    note(long, True, 1_749)
    assert take(long) == NONE
    note(long, True, 1_750)
    assert take(long) == WIN_TAB
    note(long, True, 2_000)
    assert take(long) == NONE, "long press must fire once"
    note(long, False, 2_051)
    assert take(long) == NONE, "long release must not also emit Win+G"

    wrapped = Shortcut()
    start = 0xFFFFFF00
    note(wrapped, True, start)
    note(wrapped, True, (start + 750) & 0xFFFFFFFF)
    assert take(wrapped) == WIN_TAB

    disabled = Shortcut()
    note(disabled, True, 100)
    note(disabled, True, 900, enabled=False)
    assert disabled == Shortcut()

    shortcut_source = SHORTCUT_C.read_text(encoding="utf-8")
    usb_source = USB_C.read_text(encoding="utf-8")
    main_source = MAIN_C.read_text(encoding="utf-8")
    cmake_source = CMAKE.read_text(encoding="utf-8")
    for snippet in (
        "PS_SHORTCUT_RELEASE_DEBOUNCE_MS 50U",
        "PS_SHORTCUT_LONG_PRESS_MS 750U",
        "now_ms - shortcut->last_high_time_ms",
        "now_ms - shortcut->press_time_ms",
        "M61_PS_SHORTCUT_WIN_TAB",
        "M61_PS_SHORTCUT_WIN_G",
    ):
        assert snippet in shortcut_source, f"missing shortcut contract: {snippet}"
    for snippet in (
        "USB_SHORTCUT_KEY_HOLD_MS 30U",
        "HID_MODIFIER_LEFT_GUI",
        "HID_KEY_TAB",
        "HID_KEY_G",
        "static volatile bool keyboard_busy",
        "usbd_hid_keyboard_in_callback",
        "keyboard_busy = false",
        "usb_busy = false",
        "if (intf == ITF_NUM_HID_KBD)",
    ):
        assert snippet in usb_source, f"missing keyboard USB contract: {snippet}"
    assert main_source.count("m61_usb_gamepad_note_controller_state(") == 2
    assert "m61_ps_shortcut.c" in cmake_source

    print("M61 PS shortcut tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
