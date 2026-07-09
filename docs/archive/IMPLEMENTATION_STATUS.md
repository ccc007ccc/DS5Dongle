# 实现状态

## 当前结论

M61 直连 DualSense 的 Classic Bluetooth HIDP 路径已经实机打通：M61 能稳定接收 `report=0x31 mode=full` 报文。仓库现在已进一步加入 M61 原生 DualSense USB composite device，并已构建、刷入和枚举成功。

此前电脑暂时没显示设备的原因不是蓝牙失败，而是没有接到 BL618 原生 `USB_DP/USB_DM`。用杜邦 USB 数据线接到原生 USB 后，Windows 已能枚举 DualSense 复合设备。M61 的板载 CH340 串口仍然只负责串口/刷写，不会变成 HID 手柄。

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
- `m61/dualsense_hidp_probe/m61_usb_gamepad.c` 新增 CherryUSB DualSense composite device：
  - VID/PID：`054C:0CE6`
  - 产品字符串：`DualSense Wireless Controller`
  - Interface 0：Audio Control
  - Interface 1：Audio Streaming OUT，EP `0x01`，4ch 16-bit 48 kHz
  - Interface 2：Audio Streaming IN，EP `0x82`，2ch 16-bit 48 kHz
  - Interface 3：DualSense HID，IN EP `0x84`，OUT EP `0x03`
  - HID report descriptor：321 字节 DualSense 描述符
  - 每次 HIDP full report 输入解析成功后推送 USB report `0x01`
- `ds5 status` 新增 USB 状态：
  - `usb_gamepad ready`
  - `configured`
  - `busy`
  - `sent`
  - `dropped`
  - `hidp_reports parsed/full/mic_audio/log`
- `tools/check_m61_hidp_log.py` 新增 `--allow-connected-stream`，允许检查“手柄已经连接后才开始抓日志”的连续 `0x31` 数据流。
- `tools/check_m61_usb_windows.py` 可检查 Windows 是否枚举到 `VID_054C&PID_0CE6` DualSense 复合设备，并区分只看到 CH340 串口的情况。
- `tools/validate_m61_usb_hardware.py` 可组合 Windows 枚举和 `ds5 status`，检查 `configured=1` 且 `sent>0`。
- M61 HIDP+USB HID 固件 WSL 构建通过。
- 新固件通过 `python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --reboot-isp` 自动进入下载并刷入成功，Flash SHA 校验通过。

## 当前实测证据

- M61 HIDP 连接稳定，曾抓到连续数百个 `report=0x31 mode=full` 报文，无断连/错误。
- 最新 HIDP+USB composite 构建通过，内存占用：
  - ROM: 601848 B / 4 MB
  - RAM: 46960 B / 415 KB
  - nocache RAM: 1808 B / 415 KB
- 最新刷写成功，工具输出：
  - `Handshake succeeded`
  - `Verification succeeded`
  - `Flash writing succeeded`
  - host/dev SHA256 一致：`fdfc521a5425fc11de723ddfd96f862f022986c6e0fd12878b7f67e3549c81f6`
- 手动 normal reset 后，`ds5 status` 显示：
  - `usb_gamepad ready=1 configured=1`
  - `usb_events ... configured=1`
  - `usb_desc dev=5 cfg=13 ...`
  - `usb_ds5 out=2 last_out=0x02/48`
  - `usb_audio open=0 close=4 ...`
- Windows PnP 实测看到：
  - `USB Composite Device`：`USB\VID_054C&PID_0CE6\M61DS5COMPOSITE1`
  - `HID-compliant game controller`：`HID\VID_054C&PID_0CE6&MI_03...`
  - `DualSense Wireless Controller` 音频控制/媒体设备
  - `扬声器 (DualSense Wireless Controller)`
  - `耳机式麦克风 (DualSense Wireless Controller)`

## 未完成

- 刷写工具 `--reset` 后仍可能停在 UART 下载口；若串口只回 `OK`，需要 `GPIO2/BOOT` 低电平时按 `RESET/RST` 正常启动。
- 还没有在蓝牙连接真实手柄后看到 `usb_gamepad sent>0` 持续增长的新日志。
- 还没有在 Windows 手柄测试面板或 Linux `jstest` 看到输入变化。
- USB HID OUT report `0x02` 已收到；当前 raw 透传到 BT `0x31`，并按 ds5-bridge 配置叠加 `trigger_reduce`、`speaker_gain`、`lock_volume` patch，但还没有实机验证 LED、普通震动、自适应扳机。
- USB Audio 已有 BT `0x39` 音频/触觉桥接路径，仍需要上板长时间验证扬声器、HD haptics 和丢帧情况。
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
   - `configured=1`：电脑已经完成 M61 原生 USB 复合设备配置。
   - `configured=0`：USB 线没有接到 BL618 原生 USB，或主机没有完成枚举。

7. 如果 `configured=0` 且电脑只显示 CH340 COM 口，需要改接 BL618 `USB_DP/USB_DM`，不能靠 CH340 串口变成手柄。
