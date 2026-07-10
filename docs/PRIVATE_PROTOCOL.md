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

## Remaining Runtime Work

Wire compatibility and persistence are implemented. These configuration
effects are not yet complete and must not be reported as finished:

- `inactive_time` disconnect policy
- `polling_rate_mode` USB scheduling policy
- `ps_shortcut_enabled` keyboard shortcut path
- `enable_wake` remote-wakeup descriptors and behavior
- full `controller_mode` DS5/DSE USB identity switching
