# M61 原生 USB 接线和排障

本文档只讨论 M61/BL616/BL618 原生 USB Device 接到 PC 的硬件链路。CH340 串口链路只能调试和刷写，不能通过固件变成 HID 手柄。

## 必须接到哪里

Ai-M61-32S v1.3.0 规格书里的相关模组脚：

| 模组脚 | 名称 | USB 线 |
| --- | --- | --- |
| 24 | `USB_DP` | D+，常见绿色线 |
| 25 | `USB_DM` | D-，常见白色线 |
| 1 或其他 GND | `GND` | GND，常见黑色线 |

USB 线颜色不是标准保证，剪线前最好用万用表或 USB 转接板丝印确认 `D+`、`D-`、`GND`。

## 供电规则

- 如果 Ai-M61-32S-Kit 已经由 CH340 USB 口供电，原生 USB 线先只接 `D+`、`D-`、`GND`，不要接第二路红色 5V。
- 如果要用原生 USB 线给板子供电，红色 5V 只能接开发板 `5V/VBUS/VIN` 供电入口，或经过稳压后供 3.3V。
- 不要把 USB 5V 直接接 Ai-M61 模组 `VCC`。规格书里的 `VCC` 是 3.3V。
- 不要把 CH340 USB 的 D+/D- 和 BL618 `USB_DP/USB_DM` 硬并联。

## 建议接线方式

最稳的开发期方式：

```text
CH340 USB 线：给板子供电 + 串口日志/刷写
原生 USB 数据线：只接 D+ / D- / GND 到 M61 USB_DP / USB_DM / GND
```

D+/D- 线尽量短，最好绞在一起。面包板跳线会增加阻抗和反射，能枚举失败时优先缩短线、换 USB2.0 线、减少转接头。

## Windows 检查

先跑：

```powershell
python tools\check_m61_usb_windows.py
```

结果解释：

- 看到 `VID_054C&PID_0CE6`、`DualSense Wireless Controller` 或 `USB Composite Device`：PC 已经枚举到 M61 原生 DualSense 复合 USB 设备。
- 只看到 `USB-SERIAL CH340 (COMx)`：当前只是串口桥；M61 原生 USB 没接到 PC。
- 看到 `未知 USB 设备(设备描述符请求失败)` / `VID_0000&PID_0002`：PC 检测到某个 USB 物理设备但读不到描述符，优先检查 D+/D- 是否接反、线是否太长、GND 是否共地、供电是否冲突。

接好原生 USB 并刷入最新 M61 HIDP+USB 固件后，跑组合 gate：

```powershell
python tools\validate_m61_usb_hardware.py -p COM5
```

通过条件：

- Windows 看到 `VID_054C&PID_0CE6`，并出现 HID game controller 与 DualSense 音频端点。
- `ds5 status` 显示 `usb_gamepad configured=1`。
- `usb_gamepad sent>0`，说明手柄输入已经发到 USB HID endpoint。

## 排障顺序

1. 断电，用万用表确认 USB 插头/转接板上的 `D+`、`D-`、`GND`。
2. 确认 `USB_DP` 接 `D+`，`USB_DM` 接 `D-`，不要反接。
3. 确认 M61 GND 和 USB GND 相连。
4. 如果 CH340 口已供电，不接原生 USB 红色 5V。
5. 线短一点，D+/D- 绞一起，减少面包板和杜邦线长度。
6. 插 PC 后运行 `python tools\check_m61_usb_windows.py`。
7. 如果仍是描述符失败，交换 D+/D- 测一次；如果变成完全无设备，再换回并检查焊点/测试点。
8. 如果 Windows 已看到 `VID_054C&PID_0CE6`，但 `validate_m61_usb_hardware.py` 没有 `usb_gamepad` 状态，刷入最新 `m61/dualsense_hidp_probe` 固件。

## 不要混淆的脚

- `GPIO2/BOOT` 是 M61 自身下载脚，不是 USB D+/D-。
- `GPIO21/GPIO22` 是 M61 UART0，常接 CH340，不是 USB D+/D-。
- ESP32 的 `GPIO0/BOOT` 与本 M61 原生 USB 链路无关。
