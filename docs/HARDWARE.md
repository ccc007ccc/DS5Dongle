# Ai-M61-32s-Kit hardware and wiring

[简体中文](HARDWARE.zh-CN.md)

## Target board

The production firmware targets the Ai-M61-32s-Kit and builds with the
Bouffalo SDK `CHIP=bl616` target. Board and module material may use BL616,
BL618, or Ai-M61-32S naming. The released BL616 image, locked board
configuration, and the physical header labels shown below are authoritative
for this project.

Do not treat another Ai-M61 module or development board as a drop-in target.
Changing the board requires revalidation of flash layout, RF parameters, USB
pins, clocks, and memory.

## Native USB header

The Ai-M61-32s-Kit exposes dedicated native-USB header labels; GPIO numbers are
not needed:

| USB cable signal | Board header |
| --- | --- |
| D+ | `USB_DP` |
| D- | `USB_DM` |
| Power | `5V` |
| Ground | `GND` |

![Ai-M61-32s-Kit native USB wiring](assets/m61-usb-wiring.png)

The onboard Type-C connector is attached to a CH340 UART bridge. It provides
UART flashing, logs, and power, but it cannot enumerate the firmware as a USB
game controller. The PC must connect to the separate `USB_DP` and `USB_DM`
headers to see:

- USB Composite Device;
- HID-compliant game controller, `VID_054C&PID_0CE6`;
- DualSense speaker/headset output;
- DualSense microphone input.

Never electrically join the CH340 USB D+/D- pair to the native
`USB_DP`/`USB_DM` headers.

## Power arrangements

For normal use, connect the four native-USB signals only: `5V`, `GND`,
`USB_DP`, and `USB_DM`. That single USB cable powers the board and carries the
game-controller data; the Type-C/CH340 cable is not required.

For flashing, unplug native USB and use only the Type-C/CH340 cable. If native
USB data must remain connected, disconnect at least its 5 V conductor so the
board has exactly one 5 V source. Do not power the board from Type-C/CH340 and
native USB at the same time.

Connect 5 V only to the header marked `5V`. Never feed 5 V into `3V3`. USB
breakout-wire colours are not guaranteed, so verify the cable pinout before
wiring.

## Recommended order for normal users

1. Leave native USB disconnected.
2. Connect only Type-C, enter BOOT+RESET ISP mode through CH340, and flash a
   Release firmware.
3. Unplug Type-C after flashing succeeds.
4. Wire `5V`, `GND`, `USB_DP`, and `USB_DM` as shown above.
5. Plug native USB into the PC; the board should power up and enumerate as a
   DualSense composite device.
6. For first pairing, put the controller into Create+PS pairing mode. A saved
   controller reconnects automatically.

See [Quick start](QUICK_START.md) for the graphical flashing and pairing
walkthrough.

## Development and diagnostics

For UART logs, Type-C/CH340 and native USB data can remain connected together,
but disconnect native USB 5 V and keep only `USB_DP`, `USB_DM`, and `GND` on
that cable. This prevents dual-source powering.

Useful validation commands:

```powershell
python tools/check_m61_usb_windows.py
python tools/validate_m61_usb_hardware.py -p COM5
```

Seeing only the COM/CH340 port means native USB is not connected. Seeing a USB
Composite Device without HID or audio children usually indicates reversed
D+/D-, a cable problem, or failed host enumeration.

## Status LED

The default Ai-M61-32s-Kit mapping is red GPIO12, green GPIO14, blue GPIO15,
active high. The normal policy is green idle, blinking blue while connecting,
and solid blue when the DualSense HIDP path is ready. Board variants must
override these definitions rather than editing runtime logic.

## Memory and clock notes

The locked release reserves 160 KiB WRAM and exposes a 319 KiB application
RAM linker region. This is the measured layout; using a pristine SDK without
the project-level `CONFIG_WRAM_LENGTH=163840` changes memory placement and is
not a performance-equivalent build.

Runtime CPU profiles are 320, 384, and 400 MHz. The default is manual 320 MHz.
Values above 400 MHz are experimental and board-dependent; they cannot be
saved as a resident profile. PBCLK and peripheral stability must be checked
independently from CPU execution.

## Safety

- Disconnect power before rewiring USB data or supply pins.
- Keep exactly one 5 V source connected while flashing.
- Never feed 5 V into `3V3`.
- Keep UART ISP available as recovery.
- Treat vendor PDF pinouts for the exact board revision as authoritative.
- This project has not been certified for commercial or safety-critical use.
