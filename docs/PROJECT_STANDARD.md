# 项目标准

## 当前目标

当前主线是 Ai-M61/BL616/BL618 单芯片方案：

```text
DualSense --Classic Bluetooth HIDP--> M61 --USB DualSense composite--> PC/主机
```

仓库只保留 M61 实现，不保留其他芯片的固件、构建入口、刷写工具或 fallback 工作流。

## 已定技术边界

- 单片机端代码优先使用 C/C++/Rust；当前 M61 固件使用 C，基于 Bouffalo SDK。
- M61 负责 Classic Bluetooth BR/EDR HIDP Host：扫描、配对、安全、SDP、L2CAP HID Control/Interrupt、DualSense bring-up、输入解析。
- M61 负责 USB Device：通过 CherryUSB 暴露 Sony `054C:0CE6` DualSense 复合设备，包含 USB Audio Control/Streaming 和 DualSense HID。

## 硬件标准

- M61 串口调试/刷写走板载 CH340/UART0：默认 `COM5 @ 115200`，模组 `TXD=GPIO21`、`RXD=GPIO22`。
- M61 自身下载脚是 `GPIO2/BOOT`：复位瞬间高电平进 UART 下载，低电平正常启动。
- M61 原生 USB 手柄输出必须走 BL618 的 `USB_DP`/`USB_DM`，不是 CH340 串口。
- 如果开发板唯一 USB 口只接 CH340，电脑只会看到串口，不会看到手柄；需要把 `USB_DP`、`USB_DM`、`GND` 接到 USB 插头或确认板载口已连接原生 USB。
- 如果 CH340 口已经给板子供电，原生 USB 数据线先不接第二路 5V；如果要由原生 USB 供电，5V 只能进开发板 `5V/VBUS/VIN` 或稳压后供电，不能直连 Ai-M61 模组 3.3V `VCC`。
- 不允许把 CH340 的 USB D+/D- 与 BL618 `USB_DP/USB_DM` 硬并联。
- 原生 USB 详细接线和排障标准记录在 `docs/M61_NATIVE_USB_WIRING.md`。

## 状态灯

M61 HIDP 固件默认按 Ai-M61-32S-Kit 当前确认的灯脚：

- 红灯：GPIO12
- 绿灯：GPIO14
- 蓝灯：GPIO15
- 高电平点亮

行为：

- 正常启动/空闲：绿灯亮
- 蓝牙连接中：蓝灯闪烁
- HIDP 连接成功：蓝灯常亮
- 红灯默认关闭

Shell 命令：

```text
m61 led status
m61 led test
m61 led auto|off|red|green|blue|connecting|connected
ds5 log [normal|quiet]
```

## 固件结构

- `m61/dualsense_hidp_probe/main.c`：M61 Classic HIDP、自动连接、shell、状态灯。
- `m61/dualsense_hidp_probe/m61_usb_gamepad.c`：USB DualSense composite device。
- `m61/dualsense_hidp_probe/dualsense_parser.c`：DualSense 输入解析。
- `m61/dualsense_hidp_probe/dualsense_output.c`：DualSense output/feature 初始化报文。
- `tools/flash_m61_firmware.py`：M61 固件刷写。
- `tools/check_m61_usb_windows.py`：Windows USB 枚举诊断，检查 `VID_054C&PID_0CE6`、DualSense HID/Audio 和 CH340。
- `tools/validate_m61_usb_hardware.py`：组合 Windows 枚举和 `ds5 status`，验证 `configured=1` 且 `sent>0`。
- `tools/capture_m61_hidp_log.py`、`tools/check_m61_hidp_log.py`：M61 HIDP 日志采集和验收。

## 完成门槛

不能把项目标记完成，直到下面证据全部成立：

1. M61 固件能自动或手动连接真实 DualSense。
2. 日志持续出现 `report=0x31 mode=full`。
3. 操作摇杆、按键、L2/R2 时，解析值和实际操作一致。
4. M61 原生 USB 被电脑枚举为 DualSense composite device，`ds5 status` 中 `usb_gamepad configured=1`。
5. `usb_gamepad sent>0` 并在手柄输入期间持续增长，Windows/Linux 手柄测试面板能看到对应输入。
6. 至少连续运行 5 分钟，无异常断连、明显延迟或持续丢包。
7. 状态灯实机行为符合上面的状态灯标准。

## 当前状态

- 新版 HIDP+USB composite 固件已构建、刷入并在手动 normal reset 后枚举成功。
- Windows 已看到 `USB Composite Device`、`HID-compliant game controller`、扬声器和麦克风，实例 ID 均属于 `VID_054C&PID_0CE6`。
- 当前剩余验证重点是 HID 输入实际映射、USB OUT 到蓝牙 SetState、自适应扳机、LED、音频/触觉和 5 分钟稳定性。
