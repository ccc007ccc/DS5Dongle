# M61 DualSense USB Adapter

这是 Ai-M61/BL616/BL618 DualSense 转 USB 项目。M61 直接通过 Classic Bluetooth HIDP 连接 DualSense，再通过原生 USB Device 枚举成 DualSense 复合 USB 设备。仓库只保留 M61 固件、工具和文档。

## 当前状态

- M61 Classic Bluetooth HIDP 已实机打通：可以连接 DualSense，并持续收到 `report=0x31 mode=full` 输入报文。
- M61 状态灯已接入：默认绿灯，蓝牙连接中蓝灯闪烁，连接成功蓝灯常亮，红灯默认关闭。
- M61 固件已加入 DualSense USB composite device：`VID_054C&PID_0CE6`，产品字符串 `DualSense Wireless Controller`，包含 Audio Control、Audio OUT、Audio IN 和 HID interface。
- 已实测 Windows 能枚举 `USB Composite Device`、`HID-compliant game controller`、DualSense 扬声器和麦克风。电脑必须接到 BL618/M61 原生 `USB_DP/USB_DM`，板载 CH340 串口不会因为固件变成手柄。
- 当前 USB Audio 只完成枚举和静音/drain 占位，HD haptics、扬声器、3.5mm、麦克风还未桥接到蓝牙 `0x36`。

## 重要硬件结论

Ai-M61-32S 模组本身有原生 USB2.0 引脚：

- `USB_DP`
- `USB_DM`

但很多 Ai-M61-32S-Kit 板子的 Type-C/USB 口主要接板载 CH340，用来做串口和刷写。如果你的电脑只看到 `COM5` 这类 CH340 串口，而没有新的 HID 设备，说明当前线缆接到的是串口桥，不是 BL618 原生 USB Device。

要让电脑看到手柄，需要满足其中一种：

- 板子的这个 USB 口确实接到了 BL618 原生 USB D+/D-。
- 或者从模块/开发板上的 `USB_DP`、`USB_DM`、`GND` 接到一个 USB 插头/转接板，再插电脑。

不要把 CH340 的 USB D+/D- 和 BL618 的 USB_DP/DM 硬并在一起。
如果板子已经由 CH340 口供电，原生 USB 数据线先只接 D+/D-/GND，不接第二路 5V。
如果要用原生 USB 线同时供电，5V 必须接开发板的 `5V/VBUS/VIN` 供电入口或经过稳压后供电，不能直接接 Ai-M61 模组 `VCC`，规格书里的 `VCC` 是 3.3V。

详细接线和排障见 [docs/M61_NATIVE_USB_WIRING.md](docs/M61_NATIVE_USB_WIRING.md)。

## M61 构建

在 Windows PowerShell 中：

```powershell
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh
```

产物：

```text
m61/dualsense_hidp_probe/build/build_out/m61_dualsense_hidp_probe_bl616.bin
```

## M61 刷写

如果当前 M61 固件已经支持 `ds5 reboot-isp`：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --reboot-isp
```

如果自动进入下载失败，手动进入 M61 UART 下载模式：

```text
按住 M61 BOOT
按一下 RESET/RST 并松开
松开 BOOT
```

然后刷：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --manual-hint
```

刷写工具的 `--reset` 目前不一定能让板子回到 normal boot。如果刷完串口只回 `OK`，说明还停在 UART 下载口；让 `GPIO2/BOOT` 保持低电平，按一下 `RESET/RST` 正常启动。

## 运行验证

先确认 Windows 是否看到了 M61 原生 DualSense 复合 USB 设备，而不是只看到 CH340 串口：

```powershell
python tools\check_m61_usb_windows.py
```

接好 BL618 原生 USB 后，可以直接跑 USB 硬件 gate：

```powershell
python tools\validate_m61_usb_hardware.py -p COM5
```

启动后先看串口状态：

```powershell
python tools\probe_m61_serial.py -p COM5 -b 115200 --dump
```

如果要验证连续 HIDP 报文：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_hidp.log --duration 20 --command "ds5 status"
python tools\check_m61_hidp_log.py m61_hidp.log --min-reports 20 --require-full-report --allow-connected-stream
```

如果手柄已经连接、串口被报文刷屏、只想读 USB 状态：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_usb_status.log --duration 3 --usb-status
```

`--usb-status` 会先发送 `ds5 log quiet`，再发送 `ds5 status`。

`ds5 status` 会打印：

```text
usb_gamepad ready=<0|1> configured=<0|1> busy=<0|1> sent=<n> dropped=<n>
usb_audio open=<n> close=<n> out_open=<0|1> in_open=<0|1> ...
hidp_reports parsed=<n> full=<n> mic_audio=<n> log=<normal|quiet>
```

含义：

- `configured=1`：电脑已经完成 USB 复合设备配置。
- `sent>0` 或持续增长：M61 正在把 DualSense 输入送到 USB HID endpoint。
- `dropped` 增长且 `configured=0`：蓝牙输入正常，但 USB 端没有被电脑枚举。

Windows 桌面测试程序：

```powershell
python tools\ds5_windows_test_app.py
```

这个工具会枚举 `VID_054C&PID_0CE6`，读取 HID input report `0x01`，显示摇杆、按键、触摸板、IMU、电量等字段，并可发送 USB output report `0x02` 测试 LED、普通震动和自适应扳机字段。

## 当前源码入口

- `m61/dualsense_hidp_probe/main.c`：Classic BT HIDP 连接、配对、SDP、L2CAP、输入解析、自动重连、状态灯。
- `m61/dualsense_hidp_probe/m61_usb_gamepad.c`：CherryUSB DualSense 复合 USB 描述符、Audio 占位和 HID 输入/输出报文。
- `m61/dualsense_hidp_probe/dualsense_parser.c`：DualSense 0x31/0x01 输入报文解析。
- `m61/dualsense_hidp_probe/dualsense_output.c`：DualSense 蓝牙 output/feature 初始化报文构造。
- `tools/check_m61_usb_windows.py`：Windows 侧 USB 枚举检查，区分 DualSense 复合设备与 CH340 串口。
- `tools/ds5_windows_test_app.py`：Windows Tkinter 桌面测试工具，直接用 Windows HID API 读写 DualSense HID interface。
- `tools/validate_m61_usb_hardware.py`：组合 Windows 枚举和 `ds5 status` 的 M61 原生 USB 硬件 gate。

## 离线检查

```powershell
python tools\run_offline_checks.py
```

构建 M61 固件：

```powershell
python tools\run_offline_checks.py --include-m61-build
```
