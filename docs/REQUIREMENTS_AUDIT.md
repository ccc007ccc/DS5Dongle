# Requirements Audit

本文档按两个来源审计当前仓库：

- 原始附件规格：`C:\Users\MengChao\.codex\attachments\e710f4ea-e236-4654-af18-80e961383da9\pasted-text-1.txt`
- 后续实机调试结论：M61 已能跑通 Classic Bluetooth HIDP，当前主线改为 M61 单芯片直连 DualSense 并输出 USB HID。

后续指令和硬件证据已经覆盖了原附件中的“必须先 ESP32、BL618 不做蓝牙”阶段边界。当前审计以 `docs/PROJECT_STANDARD.md` 的 M61-first 标准为准，同时保留 ESP32 双芯片方案作为 fallback 记录。

## 总体结论

当前仓库已经迁移为 Ai-M61/BL616/BL618 DualSense USB Adapter：

```text
DualSense --Classic Bluetooth HIDP--> M61 --USB HID Gamepad--> PC/主机
```

已完成的核心实现：

- M61 Classic BR/EDR HIDP Host：扫描、自动直连、配对、安全、SDP、L2CAP HID Control/Interrupt。
- DualSense `report=0x31 mode=full` 输入解析和实机持续数据流。
- M61 状态灯策略：启动绿灯、连接中蓝灯闪、连接成功蓝灯常亮、红灯默认关闭。
- M61 原生 USB HID Gamepad Device 代码和构建配置。

仍缺少的完成证据：

- PC 通过 BL618 原生 `USB_DP/USB_DM` 枚举到 `M61 DualSense Gamepad`。
- `ds5 status` 显示 `usb_gamepad configured=1`，且 `sent>0` 或持续增长。
- Windows/Linux 手柄测试工具验证按键、摇杆、L2/R2 映射和延迟。
- 连续 5 分钟端到端稳定性。

## 当前标准覆盖

| 要求 | 当前状态 | 证据 |
| --- | --- | --- |
| 仓库主线改为 M61 直连链路 | 已完成 | `README.md`、`docs/PROJECT_STANDARD.md` |
| 移除旧 Pico/RP2040/RP2350 主线 | 已完成 | `src/`、`boards/`、`lib/`、`cmake/`、`.gitmodules` 不存在 |
| M61 Classic HIDP Host | 已实现并实机打通 | `m61/dualsense_hidp_probe/main.c`，运行日志出现连续 `report=0x31 mode=full` |
| DualSense 输入解析 | 已实现 | `main/dualsense_parser.c`、`tools/test_dualsense_protocol.py` |
| 自动连接/保存地址 | 已实现 | `ds5 auto`、`ds5 autoconnect`、EasyFlash 保存 last address |
| M61 状态灯 | 已实现 | `m61 led ...`，默认 GPIO12/14/15，高电平点亮 |
| M61 USB HID Gamepad | 已实现，待原生 USB 实机枚举 | `m61/dualsense_hidp_probe/m61_usb_gamepad.c` |
| CH340 与原生 USB 边界 | 已记录 | `README.md`、`docs/PROJECT_STANDARD.md` |
| ESP32 双芯片方案 | 保留为 fallback/历史工具 | `main/`、`tools/*stage1*`、`m61/esp32_prog_bridge` |
| Arduino 禁止 | 已覆盖 | `tools/verify_project.py` 禁止 `.ino` |

## 原附件要求的处理

| 原附件要求 | 当前处理 |
| --- | --- |
| ESP32-WROOM-32 作为蓝牙主控 | 保留 fallback 代码，不再是默认推进方向 |
| Ai-M61/BL618 作为 USB 输出端 | M61 当前既负责蓝牙 HIDP Host，也负责 USB HID Device |
| ESP32 UART2 -> BL618 UART 产品链路 | 当前不实现，除非 M61-only 被硬件证据否定 |
| BL618 SDK 不支持经典蓝牙 | 已被本地 SDK 库和实机 HIDP 数据流推翻 |
| 阶段 1 未过不写 BL618 USB | 后续 M61-only 指令覆盖；M61 USB HID 已实现并构建刷入 |
| ESP32 不做 USB Device/OTG | 仍成立，ESP32 fallback 不作为 USB 输出端 |
| MCU 代码优先 C/C++/Rust | 当前固件为 C |

## 验收门槛

不能把完整目标标记完成，直到下面证据全部成立：

1. M61 固件能自动或手动连接真实 DualSense。
2. 日志持续出现 `report=0x31 mode=full`。
3. 操作摇杆、按键、L2/R2 时，解析值和实际操作一致。
4. PC 通过 BL618 原生 `USB_DP/USB_DM` 枚举到 HID Gamepad，`ds5 status` 中 `usb_gamepad configured=1`。
5. `usb_gamepad sent>0` 并在手柄输入期间持续增长，Windows/Linux 手柄测试面板能看到对应输入。
6. 至少连续运行 5 分钟，无异常断连、明显延迟或持续丢包。
7. 状态灯实机行为符合 `docs/PROJECT_STANDARD.md`。

## 审计命令

离线结构和标准检查：

```powershell
python tools\run_offline_checks.py
```

Windows USB 枚举检查：

```powershell
python tools\check_m61_usb_windows.py
```

M61 原生 USB 硬件 gate：

```powershell
python tools\validate_m61_usb_hardware.py -p COM5
```

带真实 M61 日志的审计：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_hidp.log --duration 20 --command "ds5 status"
python tools\audit_requirements.py --require-spec --m61-log m61_hidp.log
python tools\audit_requirements.py --require-spec --m61-log m61_hidp.log --strict-complete
```

如果串口报文刷屏导致 `ds5 status` 不易捕获，先静音报文日志：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_usb_status.log --duration 3 --usb-status
```

`--strict-complete` 只有在日志和人工端到端证据都满足时才应该返回成功。当前 USB 原生 D+/D- 未确认前，审计必须保留 pending 项。
