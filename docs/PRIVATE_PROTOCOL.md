# Private Configuration Protocol

Authoritative upstream: `awalol/DS5Dongle` commit
`ea93fad59a8f74e49f649a59005dc8b1a6b87a70`, specifically
`src/cmd.cpp`, `src/config.h`, and `src/config.cpp`.

## HID Feature Reports

| Report | Direction | Payload |
| --- | --- | --- |
| `0xF6` | SET | byte 0 command: `0x01` update body, `0x02` save, `0x03` USB reconnect |
| `0xF7` | GET | raw 20-byte `Config_body` |
| `0xF8` | GET | firmware version bytes, without a forced terminator |
| `0xF9` | GET | byte 0 signed RSSI; optional byte 1 audio-valid/status flags |

`0xF6/0x01` copies at most 20 bytes and preserves the existing tail when a
short body is supplied, matching upstream `set_config()` behavior. A config
update immediately queues a controller state update for mic selection,
trigger reduction, and speaker pre-gain.

## Config Body

The structure is packed and exactly 20 bytes:

| Offset | Type | Field | Range |
| --- | --- | --- | --- |
| 0 | `u8` | `config_version` | `5` |
| 1 | `float32 LE` | `haptics_gain` | 1.0..2.0 |
| 5 | `u8` | `speaker_volume` | 0..127 |
| 6 | `u8` | `headset_volume` | 0..127 |
| 7 | `u8` | `speaker_gain` | 0..7 |
| 8 | `u8` | `inactive_time` | 0..60 minutes |
| 9 | `u8` | `disable_pico_led` | 0/1 |
| 10 | `u8` | `polling_rate_mode` | 0=250 Hz, 1=500 Hz, 2=realtime |
| 11 | `u8` | `audio_buffer_length` | 16..128 |
| 12 | `u8` | `controller_mode` | 0=DS5, 1=DSE, 2=auto |
| 13 | `u8` | `enable_usb_sn` | 0/1 |
| 14 | `u8` | `ps_shortcut_enabled` | 0/1 |
| 15 | `u8` | `mic_select` | 0=auto, 1=builtin, 2=headset, 3=disabled |
| 16 | `u8` | `speaker_select` | 0=auto, 1=builtin, 2=headset, 3=disabled |
| 17 | `u8` | `enable_wake` | 0/1 |
| 18 | `u8` | `trigger_reduce` | 0..10 |
| 19 | `u8` | `lock_volume` | 0/1 |

The default body vector is:

```text
05 00 00 80 3f 64 64 02 1e 00 01 40 02 00 00 00 00 00 00 00
```

## Persistent Storage

EasyFlash key: `ds5_bridge_cfg`.

The stored C layout matches upstream `Config`: magic at offset 0, CRC32 at
offset 4, body size at offset 8, body at offset 10, then two alignment bytes.
Total size is 32 bytes.

- magic: `0x66CCFF00`
- size: `20`
- CRC: upstream reflected CRC32 with the `0xA2` seed, over body bytes only
- default-body CRC test vector: `0x457C81FC`

Load validates length, magic, size, and CRC. Save reads the value back and
verifies the same fields and body bytes. The previous body-only 20-byte format
is migrated; legacy `disable_mic/disable_speaker=1` becomes select value `3`.

## Runtime Behavior

Wire compatibility, persistence, and the configuration effects below are
implemented.

`polling_rate_mode` updates the HID IN/OUT endpoint interval on USB reconnect.
`inactive_time` disconnects a controller after the configured number of
minutes with all four stick axes in the upstream 120..140 neutral window,
both triggers released, D-pad neutral, and no buttons pressed. `0` disables
the policy. The disconnect is dispatched by the bridge task so Bluetooth and
SPI receive callbacks never tear down their own transport stack.

`ps_shortcut_enabled` exposes a separate boot-keyboard HID interface on USB
reconnect. A short PS press sends `Win+G`; a press held for at least 750 ms
sends `Win+Tab`. PS release uses the upstream 50 ms low-level debounce, and
each keyboard chord is released after 30 ms. The keyboard endpoint and busy
state are independent from the DualSense HID IN/OUT endpoints.

`controller_mode` snapshots one complete USB identity on each USB init or
reconnect. DS5 uses PID `0x0CE6`, product string `DualSense Wireless
Controller`, and the upstream 321-byte HID report descriptor. DSE uses PID
`0x0DF2`, product string `DualSense Edge Wireless Controller`, and the
upstream 437-byte descriptor; the configuration descriptor's HID report
length is changed with the selected descriptor. Forced DS5/DSE modes select
their identity immediately. Auto mode waits up to 500 ms after the first full
controller report for the Edge `0x70` feature probe before the first USB
enumeration. The wait actively sends a fresh `0x70` request and records its
attempt, pending state, request error, and either an Edge result or an explicit
timeout-to-DS5 result. The selected identity remains stable until the next
reconnect. In dual-chip mode, ESP32 BT state transitions reset Edge detection
on disconnect and at the start of a new controller generation, including a
change of controller address, so auto mode cannot inherit an old Edge result.

`enable_wake` snapshots USB 2.1 descriptors on reconnect, advertises remote
wakeup, and exposes the same keyboard interface even when PS shortcuts are
disabled. The BOS platform capability provides the 88-byte Microsoft OS 2.0
`SelectiveSuspendEnabled=1` property for the audio function so Windows can
suspend the composite device. Controller button/D-pad activity requests a
CherryUSB remote wake. After resume, the keyboard sends the upstream F15
keydown/keyup sequence twice so Windows consumes input during its wake window.
