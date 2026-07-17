# Protocol and audio formats

[简体中文](PROTOCOL.zh-CN.md)

This document describes the formats used by the M61 firmware. It is not a
complete Sony protocol specification.

## USB identity

| Field | Value |
| --- | --- |
| Vendor/Product | `054C:0CE6` |
| Product string | `DualSense Wireless Controller` |
| HID interface | 3 |
| HID IN/OUT | `0x84` / `0x03` |
| Audio OUT/IN | `0x01` / `0x82` |

The composite descriptor contains Audio Control, Audio Streaming OUT, Audio
Streaming IN, and HID interfaces.

## Input reports

Bluetooth full input uses report `0x31`. The parser accepts the HIDP report
with or without the `0xA1` transaction byte and can parse short `0x01` reports
during bring-up. Only `0x31` contains the complete touch, IMU, battery, and
device-state fields required by the production path.

The M61 converts the common payload to the wired USB input layout before
sending it to the PC. Bluetooth CRC bytes and transport headers are not
exposed as USB input data.

## Output and Feature reports

USB output state is merged into the controller's 63-byte Bluetooth state and
sent through Bluetooth reports `0x31`/`0x32`/`0x36` as appropriate. CRC32 uses
the reflected polynomial `0xEDB88320`; the release implementation uses a
bit-exact 16-entry nibble table.

USB Feature GET/SET is a transparent proxy:

- the Report ID remains byte 0 in the returned USB buffer;
- multiple GETs use a bounded FIFO with duplicate coalescing;
- SETs are forwarded before dependent GETs;
- SET `0x80` invalidates and refreshes dynamic GET `0x81`;
- the cache is cleared when the Bluetooth link or USB session resets.

This proxy is why controller firmware, factory, MAC, and telemetry pages can
be read by normal DualSense host software.

## USB Audio OUT

| Property | Value |
| --- | --- |
| Sample rate | 48,000 Hz |
| Width | signed 16-bit little-endian PCM |
| Channels | 4, interleaved |
| Channel 0/1 | speaker/headset left and right |
| Channel 2/3 | HD-haptics left and right |
| Nominal USB packet | 384 bytes per 1 ms |
| Descriptor maximum | 392 bytes |

Speaker PCM is collected in 512-frame epochs and converted to a 480-sample,
10 ms Opus frame. The release encoder is fixed-point, CBR, complexity 0,
medium-band, 160 kbit/s, with one or two forced channels according to the
runtime route. The encoded block is padded to 200 bytes for the controller
transport. These parameters are quality/protocol invariants.

Haptics channels are decimated and converted to the controller's signed
8-bit left/right 64-byte block. Two adjacent epochs are packed into the
realtime Bluetooth audio report.

## Controller microphone and USB Audio IN

| Property | Value |
| --- | --- |
| Bluetooth payload | 71-byte Opus packet, observed fixed TOC `0xD4` |
| Decoder rate | 48,000 Hz |
| Decoder channels | 1 |
| Frame | 480 samples / 10 ms |
| USB width | signed 16-bit little-endian PCM |
| USB channels | 2; mono sample duplicated to L/R |
| Nominal USB packet | 192 bytes per 1 ms |
| Descriptor maximum | 196 bytes |

The D4 fast path only handles the exact observed packet shape. PLC, FEC,
different TOCs, lengths, rates, or buffer contracts immediately fall back to
the upstream Opus parser. Host tests proved baseline and fast-path PCM
byte-identical.

## Speaker route

- `auto`: use stereo headset block `0x16` when the controller reports a
  3.5 mm headset, otherwise use mono controller-speaker block `0x13`;
- `mono`: downmix speaker channels and use `0x13`;
- `stereo`: preserve L/R and use `0x16`.

The host-facing four-channel USB format never changes during a route switch.

## M61 WebHID management protocol

The M61 configuration UI uses four vendor Feature reports. They are a native,
versioned M61 protocol and do not expose private firmware memory layouts:

- `0xF6`: apply/save configuration, reconnect USB, controller power-off,
  pair, disconnect, and forget commands;
- `0xF7`: schema-v3 configuration and capability bits, including independent
  left/right scaled radial stick deadzones;
- `0xF8`: firmware and product identity;
- `0xF9`: telemetry-v2 connection, runtime, management and bounded health
  counters. Its first eight payload bytes remain compatible with telemetry v1.

The 20-byte configuration body is stored as a CRC32-protected EasyFlash
record. Invalid records fall back to release defaults: microphone and
overclocking off, manual 320 MHz, idle shutdown disabled, and both stick
deadzones at 0%. Version-1 and version-2 records migrate with deadzones off.
