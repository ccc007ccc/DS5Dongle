# 规格演进和当前对齐

原始附件规格位于：

```text
C:\Users\MengChao\.codex\attachments\e710f4ea-e236-4654-af18-80e961383da9\pasted-text-1.txt
```

附件描述的是 ESP32-WROOM-32 + Ai-M61/BL618 双芯片方案，并要求先完成 ESP32 蓝牙阶段。后续实机调试中，M61 的 BR/EDR 能力已经被本地 SDK 和真实 DualSense HIDP 数据流证明，因此当前主线已经调整为：

```text
真实 DualSense --Classic BT HIDP--> M61 --USB DualSense composite--> PC/主机
```

## 当前硬约束

- M61 是当前默认主线；ESP32 代码只作为 fallback 和历史工具保留。
- MCU 端代码优先 C/C++/Rust；当前 M61 固件使用 C 和 Bouffalo SDK。
- M61 原生 USB 必须走 BL618 `USB_DP`/`USB_DM`；CH340 串口口不能通过固件变成手柄。
- 如果 CH340 口已经给板子供电，额外接原生 USB 数据线时先只接 D+/D-/GND，不接第二路 5V。
- USB 5V 不能直连 Ai-M61 模组 `VCC`，规格书里的 `VCC` 是 3.3V。
- ESP32-WROOM-32 没有原生 USB Device/OTG；即使回退双芯片方案，ESP32 也不做 USB 输出端。
- 不使用 Arduino `.ino` 作为 MCU 固件入口。

## 与原附件的差异

原附件中“BL618 不支持经典蓝牙 BR/EDR”的判断已经不符合当前证据：

- 本地 `bl_mcu_sdk` 含 `ble1m2s1bredr1` 控制器库。
- M61 固件可使用 `bt_conn_create_br()`、SDP、BR/EDR L2CAP。
- 实机已连接 DualSense，并持续收到 `report=0x31 mode=full`。

因此原先“必须先 ESP32，BL618 只做 USB”的阶段门槛被后续 M61-only 指令覆盖。保留 ESP32 路线的原因是风险兜底，而不是当前默认开发顺序。

## 当前实现边界

已实现：

- M61 Classic Bluetooth HIDP Host。
- DualSense 0x31/0x01 输入解析。
- DualSense output/feature 初始化报文构造。
- EasyFlash 保存上次 DualSense 地址，启动自动直连或扫描。
- M61 状态灯。
- CherryUSB DualSense composite device，VID/PID `054C:0CE6`，产品字符串 `DualSense Wireless Controller`。
- USB interface 0-3 分别为 Audio Control、Audio Streaming OUT、Audio Streaming IN、DualSense HID。
- USB HID OUT `0x02` 的 `SetStateData` 状态缓存和 flag 合并。

已实测：

- 当前硬件通过杜邦 USB 线把 PC 连接到 BL618 原生 `USB_DP`/`USB_DM`。
- Windows 枚举到 `VID_054C&PID_0CE6` 的 USB Composite、HID game controller、扬声器和麦克风。
- `ds5 status` 显示 `usb_gamepad configured=1`。

待验证：

- 蓝牙连接后 `usb_gamepad sent` 随真实手柄输入增长。
- PC 端手柄测试工具中的按键、摇杆、L2/R2 映射。
- 5 分钟端到端稳定性。

## fallback 边界

ESP32 stage-1、M61 ESP32 programming bridge 和 UART 双芯片链路文档只用于 fallback。除非 M61 Classic HIDP 或原生 USB 被硬件证据否定，否则不把新功能投入回 ESP32 主线。
