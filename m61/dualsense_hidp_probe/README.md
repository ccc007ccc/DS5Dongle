# M61 production firmware

[简体中文](README.zh-CN.md) · [Project documentation](../../README.md)

This directory is the production BL616-target firmware. It contains the
single-chip Bluetooth HIDP/USB composite bridge, the locked release build,
and the tracked Opus optimization patches.

## Build

From a checkout with the locked SDK and toolchain:

```powershell
.\build_windows.ps1 -Command All -SdkPath C:\work\bl_mcu_sdk -ToolchainBin C:\work\toolchain_gcc_t-head_windows\bin
```

The default is the performance-qualified release profile. The script verifies
dependencies, prepares Opus from a clean archive, builds BIN/ELF/MAP, and
writes a provenance manifest. See [Building](../../docs/BUILDING.md).

## Runtime commands

```text
ds5 help
ds5 status
ds5 scan | pair | autoconnect | connect <address>
ds5 disconnect | forget
ds5 mic on|off
ds5 speaker auto|mono|stereo
ds5 log normal|quiet
ds5 reboot-isp

m61 help
m61 clock status
m61 clock profile eco|balanced|performance
m61 clock lock <320..400>
m61 clock governor manual|realtime
m61 clock boost <320..400> <hold-ms>
m61 clock save | clear-saved
```

Raw Feature/send, decoder-benchmark, memory-benchmark, and profile commands
are for development. Use `ds5 help` as the exact command authority.
`ds5 log normal` prints every input report and can materially reduce HID
throughput; the release default is quiet. Software ISP reboot is best effort,
while physical BOOT+RESET is the reliable recovery path.

## Source map

- `main.c`: board startup, Bluetooth HIDP, pairing, reconnect, shell and bridge;
- `m61_usb_gamepad.c`: CherryUSB composite device, audio and codec execution;
- `m61_audio_epoch.c`: speaker/haptics epoch ownership;
- `m61_bt_tx_scheduler.c`: Bluetooth TX policy;
- `m61_dvfs.c`: runtime frequency policy and persistence;
- `m61_web_config.c`: WebHID configuration and Flash record;
- `m61_stick_deadzone.c`: scaled radial stick deadzones;
- schema-v5 settings persist hardware-PWM LED brightness and the DualSense
  audio-buffer hint while migrating schema-v1 through schema-v4 records;
- `dualsense_parser.c` / `dualsense_output.c`: controller protocol;
- `patches/`: reviewable Opus 1.2.1 performance patch stack.

Do not copy an SDK or generated Opus tree into this directory.
