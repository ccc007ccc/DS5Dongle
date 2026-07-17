# Features and current gaps

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
| DVFS | Persist/clear resident frequency policy | Verified | EasyFlash record; experimental clocks cannot be saved |
| Diagnostics | `ds5 status` full transport/audio counters | Verified | Queue, codec, USB, BT, Feature proxy and haptics counters |
| Diagnostics | Compile-gated HPM/pipeline/runtime profiling | Verified | Disabled in release |
| Recovery | UART ISP software reboot and flashing tool | Verified | Manual BOOT/RESET remains the recovery path |
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
| Console report logging | Quiet/normal selectable |

The default favors a smooth speaker/haptics path. Enabling microphone decode
adds a second Opus workload. Full-duplex operation is functional, but 320 MHz
does not yet have the same subjective stutter margin as speaker-only mode.
No sample rate, bit depth, bitrate, channel, frame-length, or frequency-band
reduction is accepted as a performance fix.

## Not yet implemented

These are real product gaps, not hidden build options:

- the Pico firmware's WebHID configuration reports `0xF6`–`0xF9`;
- a versioned, unified persistent configuration for audio route, mic state,
  polling mode, controller mode, USB serial, wake, shortcuts, gains, and idle
  timeout;
- browser-triggered firmware update/USB DFU (M61 currently uses UART ISP);
- selectable USB HID polling rate and DualSense/DualSense Edge mode;
- Windows wake/Game Bar shortcut emulation;
- firmware-side haptics/speaker gain and adaptive-trigger reduction settings;
- web-visible live performance counters and runtime DVFS/audio controls;
- DualSense Edge (`054C:0DF2`) end-to-end hardware support;
- long-duration full-duplex qualification with zero audible stutter at the
  default 320 MHz profile.

The Web migration plan and firmware/Web ownership decisions live in the
separate configuration-web repository specification.

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
