# M61 DualSense Dongle

[简体中文](README.zh-CN.md)

An open-source, single-chip DualSense-to-USB bridge for the Ai-M61-32S
(BL616/BL618 family). The M61 connects to a real DualSense over Bluetooth
Classic HIDP and exposes a native USB `054C:0CE6` DualSense composite device
to the host PC.

```text
DualSense -- Bluetooth Classic HIDP --> M61 -- native USB --> PC
```

This is an independent community project. It is not affiliated with or
endorsed by Sony Interactive Entertainment. “DualSense” is a trademark of
its respective owner.

## Current capabilities

| Area | Implemented |
| --- | --- |
| Bluetooth | BR/EDR discovery, pairing/security, SDP, HID control/interrupt L2CAP, saved-device reconnect |
| USB HID | DualSense descriptors and input reports, output reports, complete Feature GET/SET proxy |
| Controller output | Light bar/player LEDs, mute LED, standard rumble, adaptive triggers |
| Audio and haptics | 48 kHz/16-bit four-channel USB OUT, speaker/headset routing, HD haptics, Opus transport |
| Microphone | DualSense Opus decode to 48 kHz/16-bit stereo USB IN (duplicated controller mono) |
| Runtime controls | Persistent audio routing/buffer hint, hardware-PWM LED brightness, 320/384/400 MHz DVFS, stick deadzones, idle power-off, and 250/500 Hz USB report modes |
| WebUI | Versioned WebHID configuration, Flash persistence, controller management, and bounded telemetry |
| Diagnostics | Serial status/bring-up commands, compile-gated HPM/pipeline/runtime profiling, host validation tools |

The production defaults are conservative and deterministic: 320 MHz manual
CPU mode, microphone disabled, automatic speaker route, no overclock, no stick
deadzone, 12% status-LED brightness, audio buffer length 48, no inactivity
power-off, and realtime Bluetooth report forwarding.
See [Features and limitations](docs/FEATURES.md) for validation status and
known constraints.

Configure these persistent settings from the M61-specific WebHID application:
<https://ds5.766677.xyz/>. Use a Chromium-based
browser over HTTPS, connect the M61, read the current configuration, then save
the complete configuration to Flash. The USB report choices are realtime
fresh-report forwarding and hardware-validated fixed 250 Hz or 500 Hz modes;
fixed modes may repeat the latest Bluetooth sample and do not raise the
controller's native sampling rate.

The supported controller target is the standard DualSense. DualSense Edge is
not supported or hardware-qualified. During long wireless speaker playback,
the sound can occasionally become temporarily muffled and later recover; this
known limitation does not affect USB enumeration or controller input.

## Using a prebuilt Release

Normal Windows users only need `M61-Flasher-Windows.exe` from the project
[Releases](https://github.com/ccc007ccc/DS5Dongle/releases). Double-click it to
open the graphical interface, select a firmware version from the complete
verified Release list, and follow
the BOOT+RESET instructions. It detects the M61 CH340 port, downloads and
checks the selected complete firmware ZIP, and can download the signed
official WCH driver when the connected CH340 has no usable COM port.

No repository clone, Python, Rust, SDK, or compiler is required. The community
EXE is not Authenticode code-signed, so Windows SmartScreen may ask for
confirmation. Download it only from this repository and verify the SHA256
published beside it. The manual three-BIN developer path remains documented in
[Building and flashing](docs/BUILDING.md).

The flasher GUI supports Simplified Chinese and English, defaults from the
Windows user locale, and provides an in-app language selector.
For offline or custom flashing, select a downloaded
`M61-Firmware-<version>.zip` directly; an advanced three-file directory option
is also available. The log supports mouse selection and a Copy all button.

## Performance-reproducible build

The default build is the measured release profile, not an SDK fallback:

- Bouffalo SDK `2.3.24` at commit
  `d9306a4a221db414131337ec95113e3adaf7072b`;
- Xuantie/T-Head GCC 10.2.0 V2.6.1 at a pinned platform commit;
- Opus 1.2.1 downloaded by SHA256 and rebuilt with the tracked 11-patch E907
  optimization stack;
- `m61_usb_gamepad.c` at `-O2`, Opus at `-O2 -flto`, 160 KiB WRAM, 1 ms
  codec window, Flash-resident nibble CRC, and decode-MDCT SRAM placement;
- HPM, pipeline, task-runtime, and Opus-stage instrumentation disabled.

`build_windows.ps1` and `build.sh` reject an unpinned SDK/toolchain by default.
Every successful build emits a JSON provenance manifest beside the firmware.
The complete lock is
[`reproducible-build.lock.json`](m61/dualsense_hidp_probe/reproducible-build.lock.json).

## Developer build (Windows)

Clone the three repositories at their locked commits:

```powershell
git clone https://github.com/ccc007ccc/DS5Dongle.git
git clone https://github.com/bouffalolab/bl_mcu_sdk.git
git -C bl_mcu_sdk checkout d9306a4a221db414131337ec95113e3adaf7072b
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_windows.git
git -C toolchain_gcc_t-head_windows checkout 072fc29d765774d66366c57a4d962e90c366ef1b

cd DS5Dongle\m61\dualsense_hidp_probe
.\build_windows.ps1 -Command All `
  -SdkPath C:\path\to\bl_mcu_sdk `
  -ToolchainBin C:\path\to\toolchain_gcc_t-head_windows\bin
```

The script downloads and verifies Opus automatically. Release artifacts are
written to:

```text
m61/dualsense_hidp_probe/build-win/build_out/
  m61_dualsense_hidp_probe_bl616.bin
  m61_dualsense_hidp_probe_bl616.elf
  m61_dualsense_hidp_probe_bl616.map
  m61_dualsense_hidp_probe_bl616.manifest.json
```

Read [Building and flashing](docs/BUILDING.md) before overriding any release
option. An override intentionally produces a `custom` manifest and is not a
performance-equivalent release build.

## Hardware

The PC must connect to the BL616/BL618 native `USB_DP` and `USB_DM` pins. The
USB connector on many Ai-M61 development boards is wired to a CH340 UART and
cannot enumerate the firmware as a game controller. Follow
[Hardware and wiring](docs/HARDWARE.md); never electrically join CH340 USB
data lines with the SoC native USB pins.

## Flash and verify

Normal users run `M61-Flasher-Windows.exe`, select a firmware Release, and
follow its on-screen instructions. Source development and hardware diagnostics
use the repository commands below:

```powershell
python tools\flash_m61_firmware.py -p COM5 --windows-build
python tools\check_m61_usb_windows.py
python tools\validate_m61_usb_hardware.py -p COM5
```

The normal flasher default is 460800 baud. Select the board's CH340 UART
(`1A86:7523`, typically COM5), not another transient USB serial port. Add
`-b 115200` only as a conservative fallback for an unstable cable or hub.

`--reboot-isp` is a best-effort development convenience: some BL616 boards
warm-reset into BootROM but then fail its eFuse read. Holding BOOT, tapping
RESET, and releasing BOOT is the reliable recovery and flashing procedure.

## Documentation

- [Features and limitations](docs/FEATURES.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Building and flashing](docs/BUILDING.md)
- [Hardware and wiring](docs/HARDWARE.md)
- [Protocol and audio formats](docs/PROTOCOL.md)
- [Performance and benchmark policy](docs/PERFORMANCE.md)
- [Development and validation](docs/DEVELOPMENT.md)
- [Dependencies, licensing, and redistribution](docs/OPEN_SOURCE.md)

## License

Project-owned source and documentation are released under the
[MIT License](LICENSE). External SDKs and Opus retain their own licenses; see
[Third-party notices](THIRD_PARTY_NOTICES.md).
