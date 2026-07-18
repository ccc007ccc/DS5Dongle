# Architecture

[简体中文](ARCHITECTURE.zh-CN.md)

## System boundary

The production target is one M61/BL616-class SoC. FreeRTOS, the Bouffalo
Bluetooth stack, CherryUSB, and the application all remain in one firmware
image.

```text
                       M61 / BL616
DualSense  <---->  Bluetooth HIDP transport
                         |
                         v
                 report/audio bridge
                         |
                         v
PC          <---->  CherryUSB composite device
```

## Realtime data paths

### Controller input

1. The Bluetooth HID interrupt callback receives report `0x31`.
2. `dualsense_parser.c` validates and decodes the full state.
3. The latest state is converted to the wired USB input layout.
4. The USB input pump sends the newest state when endpoint `0x84` is free.

Input is latest-state data: an old queued sample is never more valuable than
the current controller state.

### Controller output and Feature reports

1. CherryUSB receives host output/Feature SET requests.
2. The application copies them into bounded queues; USB callbacks do not wait
   on Bluetooth allocation.
3. The central Bluetooth TX scheduler gives realtime audio, state, Feature,
   and diagnostic traffic explicit classes.
4. Bluetooth Feature responses are cached with their Report ID intact and
   returned to the original USB GET request. Dynamic `0x81` pages are
   invalidated by the corresponding `0x80` SET.

### Speaker and HD haptics

```text
USB OUT 48 kHz / 16-bit / 4 channels / 1 ms packets
        |
        +--> ch0/ch1 --> 512-frame epoch --> exact 512:480 resample
        |                                      --> Opus encode
        |
        +--> ch2/ch3 --> HD-haptics PCM
                               |
                               v
              paired 10 ms epochs --> BT report 0x36
```

The route policy selects mono controller speaker, stereo headset, or an
explicit stereo/mono override. Routing never changes host sample rate or bit
depth.

### Microphone

```text
BT report 0x36, fixed 71-byte D4 Opus frame
        --> bounded Opus queue
        --> Opus decode (10 ms)
        --> mono PCM duplicated to stereo
        --> 10 x 1 ms USB Audio IN packets
```

Mic processing can be disabled at runtime without changing USB enumeration;
Windows continues receiving intentional silence.

## Scheduling and ownership

- SDK Bluetooth tasks own controller callbacks and protocol progress.
- USB callbacks only reserve/copy/publish bounded data.
- The codec task is the only writer of codec timing and encode/decode state.
- Audio epochs have generation IDs and explicit ownership transitions.
- The Bluetooth TX scheduler is the only policy point for realtime and
  control packet admission.
- DVFS requests are posted to a worker; no audio callback reprograms PLLs.

The design is event-driven where the SDK exposes a reliable event. Short
service windows remain where the half-complete SDK offers no completion
credit callback; these windows are explicit and benchmarked.

## Memory and code placement

The release linker layout reserves 160 KiB WRAM, matching the measured
firmware, and leaves a 319 KiB application RAM region. Hot Opus PVQ/MDCT
clusters and decode MDCT are selectively placed in cacheable SRAM. Broadly
moving code into SRAM is forbidden: hardware tests showed that larger code
working sets can increase I-cache conflict and tail latency.

The release memory gate limits boot-critical TCM and static physical RAM.

The BL616 HID IN endpoint uses the locked SDK's stock Full-Speed PHY/VDMA
completion path. The project does not replace the shared SDK USB interrupt
handler, FIFO layout, audio endpoints, or 64-byte DualSense report size.
Descriptor intervals and latest-sample scheduling are selected by the M61
configuration; the published 250 Hz and 500 Hz values are host-observed rates
measured on hardware, not a claim that the Bluetooth controller samples at a
higher rate.
Diagnostic buffers are compiled only into their corresponding profile.

## Main source modules

| Module | Responsibility |
| --- | --- |
| `main.c` | Board startup, Bluetooth HIDP, pairing/reconnect, Feature bridge, shell |
| `m61_usb_gamepad.c` | USB descriptors, endpoints, audio ingress/egress, Opus codec task |
| `m61_audio_epoch.c` | Speaker/haptics epoch ownership and adjacent-pair assembly |
| `m61_bt_tx_scheduler.c` | Central Bluetooth TX selection and admission |
| `m61_realtime_scheduler.c` | Deadline/readiness policy |
| `m61_dvfs.c` | Runtime clock profiles, governor, persistence and worker |
| `m61_web_config.c` | Versioned WebHID configuration, migration and CRC record |
| `m61_stick_deadzone.c` | Independent scaled radial stick correction |
| `dualsense_parser.c` | Bluetooth/USB input parsing |
| `dualsense_output.c` | Bluetooth output/Feature packets and CRC |
| `m61_perf_profile.c` | Compile-gated HPM aggregation |

## Configuration boundary

The public management plane is the versioned binary WebHID protocol on Feature
reports `0xF6` through `0xF9`. Firmware advertises capabilities, validates all
ranges, applies commands asynchronously, and owns the CRC-protected Flash
record. Shell commands remain development and recovery tools; the browser does
not forward or interpret shell text.
