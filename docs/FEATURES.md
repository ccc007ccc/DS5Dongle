# Features and limitations

[简体中文](FEATURES.zh-CN.md)

Status meanings: **verified** has hardware evidence, **implemented** is covered
by source/offline tests but still needs broader hardware regression, and
**missing** has no production implementation.

## Firmware feature matrix

| Area | Capability | Status | Notes |
| --- | --- | --- | --- |
| Bluetooth | DualSense BR/EDR scan and connect | Verified | Name filter, direct address connect, saved address |
| Bluetooth | Pairing/security and bond-backed reconnect | Verified | SDK settings plus firmware last-address record |
| Bluetooth | HID SDP and L2CAP PSM `0x11`/`0x13` | Verified | Control and interrupt channels |
| Input | Full Bluetooth report `0x31` | Verified | Sticks, buttons, triggers, touch, IMU, battery, headset bits |
| USB | Sony `054C:0CE6` composite enumeration | Verified | Audio Control, Audio OUT, Audio IN, HID |
| USB HID | Input forwarding | Verified | Full DualSense-shaped USB reports |
| USB HID | Output forwarding | Verified | LED, rumble, adaptive trigger and state fields |
| USB HID | Feature GET/SET proxy | Verified | Static and dynamic controller pages retain report IDs |
| Audio OUT | Four-channel 48 kHz, 16-bit USB stream | Verified | Speaker L/R plus HD-haptics channels |
| Speaker | Mono, stereo, and automatic jack route | Verified | Auto uses controller headset state; L/R mapping corrected |
| HD haptics | USB audio to Bluetooth `0x36` | Verified | Combined with speaker packets without lowering quality |
| Microphone | Controller Opus to USB Audio IN | Verified | 48 kHz, 16-bit, two USB channels copied from mono source |
| Runtime audio | Mic enable/disable | Verified | Immediate shell/API switch; default off |
| Runtime audio | Speaker route switch | Verified | `auto`, `mono`, `stereo` |
| DVFS | Manual profiles and custom 320–400 MHz | Verified | Eco 320, balanced 384, performance 400 |
| DVFS | Experimental 401–480 MHz | Implemented | Explicit opt-in; known board stability varies |
| DVFS | Realtime governor, floors and timed boosts | Implemented | Event-driven worker, no hot-path clock writes |
| DVFS | Persist/clear resident frequency policy | Verified | Unified EasyFlash record shared with Web settings; experimental clocks cannot be saved; legacy standalone records migrate at boot |
| Diagnostics | `ds5 status` full transport/audio counters | Verified | Queue, codec, USB, BT, Feature proxy and haptics counters |
| Diagnostics | Compile-gated HPM/pipeline/runtime profiling | Verified | Disabled in release |
| Diagnostics | Controller RSSI | Missing | HCI RSSI reads on the active HID link disturb input, so production firmware does not poll it; WebUI explicitly reports it unavailable |
| WebHID | Versioned `0xF6`–`0xF9` management protocol | Verified | Capabilities, CRC-backed configuration, identity and telemetry |
| WebHID | Pair, disconnect, forget and controller power-off | Verified | Management result and sequence are reported in telemetry |
| Persistence | Unified M61 runtime configuration | Verified | Versioned EasyFlash record with CRC32 and v1 migration |
| Power | Configurable controller idle shutdown | Verified | Fixed 25% stick activity threshold excludes drift and IMU noise; disabled by default |
| Power | Controller shutdown after host suspend | Implemented | Requires final PC sleep/resume qualification |
| Input | Independent scaled radial stick deadzones | Verified | Left/right 0–30%; schema v3 and Flash persistence |
| Input | Selectable USB report rate | Verified | Realtime fresh Bluetooth reports or hardware-measured fixed 250/500 Hz latest-sample repeat; schema v4 migrates the retired experimental value to 500 Hz |
| Recovery | UART ISP software reboot and flashing tool | Implemented | Warm reboot can fail BootROM eFuse reads; manual BOOT/RESET is the reliable path |
| Board UI | RGB connection status LED | Verified | Green idle, blue connecting/connected policy |

## Production defaults

| Setting | Default |
| --- | --- |
| CPU | 320 MHz, manual governor |
| Compile-time overclock | Off |
| Microphone processing | Off |
| Speaker route | Auto: mono without a 3.5 mm headset, stereo with a headset |
| USB Audio OUT | Four-channel 48 kHz/16-bit |
| Profiling | Off |
| Console report logging | Quiet; `normal` is diagnostic-only |

The default favors a smooth speaker/haptics path. Enabling microphone decode
adds a second Opus workload. Full-duplex operation is functional, but 320 MHz
does not yet have the same subjective stutter margin as speaker-only mode.
No sample rate, bit depth, bitrate, channel, frame-length, or frequency-band
reduction is accepted as a performance fix.

## Known limitations

- The supported controller target is the standard DualSense. DualSense Edge
  is not supported or hardware-qualified.
- Controller RSSI is intentionally unavailable because active BL616 BR/EDR
  RSSI queries disturbed HID input during hardware testing.
- Microphone decode adds substantial realtime load. It is disabled by default,
  and 320 MHz full-duplex operation has less stutter margin than speaker-only.
- During long wireless speaker playback, the sound can occasionally become
  temporarily muffled and later recover. USB enumeration and controller input
  remain available.
- Host-suspend controller power-off is implemented but still needs broader PC
  sleep/resume qualification.
- Firmware updates use UART ISP. Software warm reboot is best effort; physical
  BOOT+RESET is the reliable entry and recovery method.
- `ds5 log normal` prints every HIDP input report and can materially reduce
  controller throughput. Normal operation and performance tests must use
  `ds5 log quiet` or a fresh reset.

## Serial command surface

Use `ds5 help` and `m61 help` as the runtime authority. Major groups are:

- `ds5 status`, `scan`/`pair`, `connect`, `autoconnect`, `disconnect`,
  `forget`, `security`, `sdp`, `hidp`, and `bringup`;
- `ds5 mic on|off` and `ds5 speaker auto|mono|stereo`;
- `ds5 get-feature`, `output-init`, `send-ctrl`, and `send-intr` for protocol
  diagnosis;
- `m61 clock status|profile|lock|governor|boost|save|clear-saved`;
- `ds5 reboot-isp` / `m61 reboot-isp`.

Raw send and benchmark commands are development tools and must not become a
browser-facing public API.

`m61 clock save` writes the current frequency policy together with the other
runtime settings into the unified record. `clear-saved` resets only the
frequency policy used on the next boot to manual 320 MHz; it does not change
the current session's clock.
