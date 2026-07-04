# 醒来继续 runbook

本文档用于从中断状态继续当前 M61 DualSense USB Adapter 调试。当前主线是：

```text
DualSense --Classic Bluetooth HIDP--> M61 --USB HID Gamepad--> PC/主机
```

判断依据以串口输出、USB 枚举和刷写工具校验为准，不以板载黄灯、电源灯或复位瞬间灯色为准。

## 当前边界

- M61-only 是默认主线；ESP32 只作为 fallback 和历史调试工具保留。
- M61 已能连接 DualSense 并接收 `report=0x31 mode=full`，下一步重点是 BL618 原生 USB HID 枚举。
- CH340 口只提供串口/刷写，不能因为固件改成 USB 手柄。
- M61 原生 USB HID 必须接 BL618 `USB_DP`/`USB_DM`。
- 如果 M61 串口无响应，不能靠脚本强制进入 bootrom；需要手动让 M61 `GPIO2/BOOT` 在复位瞬间为高电平。

## M61 PDF 关键结论

Ai-M61-32S v1.3.0 产品规格书中和当前调试有关的约束：

- 模组 `VCC` 是 3.3V，外部供电电流建议 `500mA` 以上。
- 模组默认串口 `RXD=GPIO22`、`TXD=GPIO21`，默认 `115200bps`。
- `GPIO2` 是 M61 自身 bootstrap 脚：复位/上电瞬间高电平进入烧录模式，低电平正常启动。
- `USB_DP` 是模组 24 脚，`USB_DM` 是模组 25 脚。
- IO 电平为 3.3V。

## 先判断电脑现在看到什么

```powershell
python tools\check_m61_usb_windows.py
```

结果解释：

- 只看到 `USB-SERIAL CH340 (COMx)`：当前插的是串口桥，不是 M61 原生 USB 手柄口。
- 看到 `VID_1209&PID_5D51` 或 `M61 DualSense Gamepad`：PC 已枚举到当前固件的 USB HID。
- 看到未知 USB 设备：优先检查 `USB_DP/USB_DM` 是否接反、线太长、供电路径是否冲突。

## 原生 USB 接线

如果当前开发板没有把 Type-C/USB 口接到 BL618 原生 USB，需要额外接 USB 数据线：

```text
USB 线绿色 D+  -> M61 USB_DP，模组 24 脚
USB 线白色 D-  -> M61 USB_DM，模组 25 脚
USB 线黑色 GND -> M61 GND
```

如果板子已经由 CH340 口供电，原生 USB 线先不要接红色 5V。
如果要让原生 USB 线同时供电，红色 5V 只能接开发板 `5V/VBUS/VIN` 供电入口，或经过稳压后供 3.3V，不能直接接 Ai-M61 模组 `VCC`。

不要把 CH340 的 D+/D- 和 M61 的 `USB_DP/USB_DM` 硬并联。

更完整的接线和描述符失败排障见 `docs/M61_NATIVE_USB_WIRING.md`。

## 无损探测 M61 串口

```powershell
python tools\probe_m61_serial.py -p COM5 --dump
```

不传 `-b` 时会依次探测 `115200`、`460800`、`2000000`。结果含义：

- `kind=hidp-probe`：M61 DualSense HIDP+USB 固件已运行，可以继续验证蓝牙和 USB。
- `kind=bridge`：M61 ESP32 调试桥已运行，这是 fallback 工具，不是当前主线固件。
- `kind=no-response`：当前正常启动固件不是可用 helper，或串口/波特率/接线不通。
- `unknown-responsive`：固件可能正在刷屏输出 HIDP 报文，命令回应被日志淹没；先抓日志再判断。

## 验证 M61 HIDP 和 USB 状态

手柄已经连上时可以直接抓状态：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_hidp.log --duration 20 --command "ds5 status"
python tools\check_m61_hidp_log.py m61_hidp.log --min-reports 20 --require-full-report --allow-connected-stream
python tools\audit_requirements.py --require-spec --m61-log m61_hidp.log
```

如果只想读 USB 状态，先关掉每帧 HIDP 日志：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_usb_status.log --duration 3 --usb-status
```

`--usb-status` 会先发 `ds5 log quiet`，再发 `ds5 status`。

接好 BL618 原生 USB 后，也可以直接跑组合验证：

```powershell
python tools\validate_m61_usb_hardware.py -p COM5
```

重点看 `ds5 status`：

```text
usb_gamepad ready=<0|1> configured=<0|1> busy=<0|1> sent=<n> dropped=<n>
hidp_reports parsed=<n> full=<n> mic_audio=<n> log=<normal|quiet>
```

- `configured=1`：PC 已枚举 BL618 原生 USB HID。
- `sent>0` 或持续增长：M61 正在把 DualSense 输入发送到 USB endpoint。
- `configured=0` 且 PC 只看到 CH340：USB 线没有接到 BL618 原生 D+/D-。
- `dropped` 增长且 `configured=0`：蓝牙输入正常，USB 主机端没完成枚举。

## 刷入 M61 HIDP+USB 固件

如果当前固件还能响应 `ds5 reboot-isp` 或 SDK shell：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --reboot-isp
```

如果自动进入下载失败，手动进入 UART 下载模式：

```text
按住 M61 BOOT
按一下 RESET/RST 并松开
松开 BOOT
```

然后刷：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --manual-hint
```

刷完如果串口只回 `OK`，说明还停在 UART 下载口；让 `GPIO2/BOOT` 保持低电平，按一下 `RESET/RST` 正常启动。

## 正常启动后的手柄流程

- 如果已经保存过 DualSense 地址，直接按手柄 `PS`，M61 会优先自动直连。
- 如果直连失败或没有保存地址，手柄进入 `PS + Create/Share` 寻找模式，M61 扫描后会保存地址。
- 连接中蓝灯闪烁，HIDP 成功后蓝灯常亮，正常空闲绿灯亮。

## ESP32 fallback

只有当 M61 Classic HIDP 或原生 USB HID 被硬件证据否定时，再回到 ESP32 fallback：

- `m61/esp32_prog_bridge`：让 M61 控制 ESP32 下载/复位。
- `tools/flash_stage1_m61.py`、`tools/flash_stage1_auto.py`、`tools/flash_stage1_manual.py`：刷 ESP32 stage-1 固件。
- `docs/M61_DEBUG_BRIDGE.md`、`docs/STAGE1_VALIDATION.md`：fallback 操作说明。

当前不要因为 USB 只看到 CH340 就回退 ESP32；这首先是 M61 原生 USB D+/D- 未接到 PC 的硬件链路问题。
