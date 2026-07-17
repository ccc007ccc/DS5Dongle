# Hardware and wiring

[简体中文](HARDWARE.zh-CN.md)

## Target identity

The firmware builds with the Bouffalo SDK `CHIP=bl616` target for the
Ai-M61-32S family. Vendor material uses both BL616/BL618 family terminology;
the build target and generated BL616 image format are authoritative for this
repository. Do not substitute another board definition without revalidating
USB pins, RF calibration, flash layout, clocks, and memory.

## Native USB is mandatory

The SoC USB device pins are:

| Signal | Firmware pin |
| --- | --- |
| USB D+ | GPIO32 / `USB_DP` |
| USB D- | GPIO33 / `USB_DM` |
| Ground | GND |

Many Ai-M61-32S-Kit USB-C connectors are wired to a CH340 serial bridge. That
connector is still useful for power, logs, and UART flashing, but firmware
cannot turn the CH340 into a game controller.

The PC must see a second native USB connection to enumerate:

- USB Composite Device;
- HID-compliant game controller, `VID_054C&PID_0CE6`;
- DualSense speaker/headset output;
- DualSense microphone input.

Never connect the CH340 D+/D- pair and the SoC D+/D- pair together.

## Power

If the board is already powered through its CH340 connector, connect only
native D+, D-, and GND from the second cable. Avoid two uncontrolled 5 V
sources.

If native USB also supplies power, feed the development board's documented
`5V`/`VBUS`/`VIN` input. Do not feed a module `VCC` pin with 5 V; module VCC
is normally 3.3 V.

## Recommended bring-up order

1. Keep CH340 connected for UART logs and flashing.
2. Wire native D+, D-, and GND to a known USB breakout/cable.
3. Build and flash the locked release firmware.
4. Reset into normal boot with BOOT released.
5. Run `python tools/check_m61_usb_windows.py`.
6. Pair/connect the controller and run
   `python tools/validate_m61_usb_hardware.py -p COM5`.

Seeing only COM5/CH340 means native USB is not connected or not configured.
Seeing the USB composite device without HID/audio children usually indicates
descriptor or host-driver enumeration failure.

## Status LED

The default Ai-M61-32S-Kit mapping is red GPIO12, green GPIO14, blue GPIO15,
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
- Do not publish an overclocked image as the default release.
- Keep UART ISP available as recovery.
- Treat vendor PDF pinouts for the exact board revision as authoritative.
- This project has not been certified for commercial or safety-critical use.
