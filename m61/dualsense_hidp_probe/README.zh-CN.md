# M61正式固件

[English](README.md) · [项目文档](../../README.zh-CN.md)

本目录是BL616目标正式固件，包含单芯片Bluetooth HIDP/USB复合桥、锁定release构建和
tracked Opus优化补丁。

## 构建

准备好锁定SDK和工具链后运行：

```powershell
.\build_windows.ps1 -Command All -SdkPath C:\work\bl_mcu_sdk -ToolchainBin C:\work\toolchain_gcc_t-head_windows\bin
```

默认就是性能验收release profile。脚本会校验依赖、从干净压缩包准备Opus、构建
BIN/ELF/MAP并生成来源manifest。详见[构建文档](../../docs/BUILDING.zh-CN.md)。

## 运行时命令

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

Raw Feature/send、decoder benchmark、memory benchmark和profile命令只供开发。精确命令
以`ds5 help`为权威。

## 源码索引

- `main.c`：板级启动、Bluetooth HIDP、配对、重连、shell和bridge；
- `m61_usb_gamepad.c`：CherryUSB复合设备、音频和codec执行；
- `m61_audio_epoch.c`：speaker/haptics epoch所有权；
- `m61_bt_tx_scheduler.c`：Bluetooth TX策略；
- `m61_dvfs.c`：运行时频率策略和持久化；
- `dualsense_parser.c` / `dualsense_output.c`：手柄协议；
- `patches/`：可审查Opus 1.2.1性能补丁栈。

不要把SDK或生成后的Opus源码树复制进本目录。
