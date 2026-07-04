# 实现状态

## 当前结论

M61 直连 DualSense 的 Classic Bluetooth HIDP 路径已经实机打通：M61 能稳定接收 `report=0x31 mode=full` 报文。仓库现在已进一步加入 M61 原生 USB HID Gamepad Device 输出，并已构建、刷入成功。

电脑暂时没显示手柄的原因不是蓝牙失败，而是 USB HID 端刚加入且仍需实机枚举验证。M61 的板载 CH340 串口不会变成 HID 手柄；电脑必须连接 BL618 原生 `USB_DP/USB_DM`。

## 已完成

- 原 Pico SDK / TinyUSB / RP2040/RP2350 旧主线已从当前工程标准中移除。
- M61 HIDP 探针实现 Classic BR/EDR inquiry、保存地址、自动直连/扫描、security、SDP、HID Control PSM `0x11`、HID Interrupt PSM `0x13`。
- M61 HIDP 探针能解析 DualSense `report=0x31 mode=full`，并打印摇杆、按键、扳机、IMU、电量。
- M61 状态灯实现：
  - 正常启动/空闲：绿灯
  - 蓝牙连接中：蓝灯闪烁
  - HIDP 连接成功：蓝灯常亮
  - 红灯默认关闭
- M61 shell 命令已包含 `ds5 status`、`ds5 auto`、`ds5 autoconnect`、`ds5 bringup`、`ds5 log quiet|normal`、`ds5 reboot-isp`、`m61 reboot-isp`、`m61 led ...`。
- `m61/dualsense_hidp_probe/m61_usb_gamepad.c` 新增 CherryUSB HID Gamepad Device：
  - VID/PID：`1209:5D51`
  - 产品字符串：`M61 DualSense Gamepad`
  - 报文：16 buttons + hat + LX/LY/RX/RY/L2/R2
  - 每次 HIDP 输入解析成功后推送 USB interrupt IN 报文
- `ds5 status` 新增 USB 状态：
  - `usb_gamepad ready`
  - `configured`
  - `busy`
  - `sent`
  - `dropped`
  - `hidp_reports parsed/full/mic_audio/log`
- `tools/check_m61_hidp_log.py` 新增 `--allow-connected-stream`，允许检查“手柄已经连接后才开始抓日志”的连续 `0x31` 数据流。
- `tools/check_m61_usb_windows.py` 可检查 Windows 是否枚举到 `VID_1209&PID_5D51`，并区分只看到 CH340 串口的情况。
- `tools/validate_m61_usb_hardware.py` 可组合 Windows 枚举和 `ds5 status`，检查 `configured=1` 且 `sent>0`。
- M61 HIDP+USB HID 固件 WSL 构建通过。
- 新固件通过 `python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --reboot-isp` 自动进入下载并刷入成功，Flash SHA 校验通过。

## 当前实测证据

- M61 HIDP 连接稳定，曾抓到连续数百个 `report=0x31 mode=full` 报文，无断连/错误。
- 最新 HIDP+USB HID 构建通过，内存占用：
  - ROM: 592288 B / 4 MB
  - RAM: 44736 B / 415 KB
  - nocache RAM: 1040 B / 415 KB
- 最新刷写成功，工具输出：
  - `Handshake succeeded`
  - `Verification succeeded`
  - `Flash writing succeeded`
  - host/dev SHA256 一致：`91b29705056214e1d4e6251ba9dd8f1caf8ff25069c29bab86b71acc530f66ea`

## 未完成

- 刷写工具 `--reset` 后仍可能停在 UART 下载口；若串口只回 `OK`，需要 `GPIO2/BOOT` 低电平时按 `RESET/RST` 正常启动。
- 还没有确认电脑是否连接到了 BL618 原生 `USB_DP/USB_DM`。
- 还没有看到 `usb_gamepad configured=1` 且 `sent>0` 的实机证据。
- 还没有在 Windows 手柄测试面板或 Linux `jstest` 看到输入变化。
- 还没有完成 5 分钟端到端稳定性和延迟观察。

## 下一步

1. 正常复位 M61，不按 BOOT。
2. 运行：

```powershell
python tools\probe_m61_serial.py -p COM5 -b 115200 --dump
```

3. 如果固件正常启动，再运行：

```powershell
python tools\check_m61_usb_windows.py
```

4. 跑组合 USB 硬件 gate：

```powershell
python tools\validate_m61_usb_hardware.py -p COM5
```

5. 或单独抓 USB 状态：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_usb_status.log --duration 3 --usb-status
```

6. 看 `ds5 status` 里的 `usb_gamepad configured`：
   - `configured=1`：电脑已经枚举到 M61 原生 USB HID。
   - `configured=0`：USB 线没有接到 BL618 原生 USB，或主机没有完成枚举。

7. 如果 `configured=0` 且电脑只显示 CH340 COM 口，需要改接 BL618 `USB_DP/USB_DM`，不能靠 CH340 串口变成手柄。
