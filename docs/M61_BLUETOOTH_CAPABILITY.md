# M61 Bluetooth Capability

本文档记录 Ai-M61/BL616/BL618 直接作为 DualSense Bluetooth Host 的当前证据。

## 当前结论

M61-only 路线已经从“可行性探针”推进为当前主线：

```text
DualSense --Classic Bluetooth HIDP--> M61 --USB DualSense composite--> PC/主机
```

本地 `bl_mcu_sdk` 不是 BLE-only。它包含 BR/EDR-capable controller libraries，例如：

- `libbtblecontroller_bl616_ble1m2s1bredr1.a`
- `libbtblecontroller_bl618dg_ble1m2s1bredr1.a`

当前 M61 固件已经实机证明：

- 能进行 Classic BR/EDR inquiry 并识别 `DualSense Wireless Controller`。
- 能发起 BR/EDR ACL 连接。
- 能完成安全/配对流程。
- 能通过 SDP 查询 HID 服务。
- 能打开 HID Control PSM `0x11` 和 HID Interrupt PSM `0x13`。
- 能完成 DualSense bring-up 并持续收到 `report=0x31 mode=full`。
- 能复用共享 parser 解析摇杆、按键、L2/R2、IMU、电量。

## SDK 缺口

SDK 没有现成 Classic HID Host/HIDP profile。当前实现依赖低层 building blocks 自己拼 HIDP：

- BR/EDR ACL connection：`bt_conn_create_br()`
- pairing/security：`bt_conn_set_security()`、`bt_conn_auth_cb_register()`
- SDP client：`bt_sdp_discover()`
- BR/EDR L2CAP client channels：`bt_l2cap_chan_connect()`
- BR/EDR L2CAP send：`bt_l2cap_chan_send()`

这意味着 M61-only 不是简单开配置开关，而是仓库中 `m61/dualsense_hidp_probe/main.c` 的自定义 HIDP Host 实现。

## 当前固件

M61 主线固件位于：

```text
m61/dualsense_hidp_probe
```

它现在包含两部分：

- Classic Bluetooth HIDP Host。
- CherryUSB DualSense composite device。

构建：

```powershell
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh
```

刷写：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --reboot-isp
```

如果自动进下载失败，手动 `BOOT`+`RESET` 进入 M61 UART 下载模式后：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 -b 115200 --manual-hint
```

运行探测：

```powershell
python tools\probe_m61_serial.py -p COM5 --dump
```

Windows USB enumeration check:

```powershell
python tools\check_m61_usb_windows.py
python tools\validate_m61_usb_hardware.py -p COM5
```

验证日志：

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_hidp.log --duration 20 --command "ds5 status"
python tools\check_m61_hidp_log.py m61_hidp.log --min-reports 20 --require-full-report --allow-connected-stream
python tools\audit_requirements.py --require-spec --m61-log m61_hidp.log
```

If report spam hides status output, capture USB status with report logs muted:

```powershell
python tools\capture_m61_hidp_log.py -p COM5 -o m61_usb_status.log --duration 3 --usb-status
```

`--usb-status` sends `ds5 log quiet` and then `ds5 status`.

## Shell 命令

当前固件提供：

```text
ds5 status
ds5 auto [on|off|now]
ds5 scan
ds5 autoconnect
ds5 connect <aa:bb:cc:dd:ee:ff|last>
ds5 security
ds5 sdp
ds5 hidp
ds5 bringup
ds5 log [normal|quiet]
ds5 set-protocol
ds5 get-feature <id_hex>
ds5 output-init
ds5 send-ctrl <hex>
ds5 send-intr <hex>
ds5 forget
ds5 disconnect
ds5 reboot-isp
m61 reboot-isp
m61 led status
m61 led test
m61 led auto|off|red|green|blue|connecting|connected
```

## USB DualSense 复合设备状态

USB DualSense 复合设备代码已加入固件：

- VID/PID：`054C:0CE6`
- 产品字符串：`DualSense Wireless Controller`
- USB interface：Audio Control、Audio OUT、Audio IN、DualSense HID
- HID：321 字节 DualSense report descriptor，interface 3，IN `0x84`，OUT `0x03`

硬件 USB 正常启动枚举已经实测打通。M61 DualSense USB 设备只能通过 BL618 原生 `USB_DP`/`USB_DM` 枚举；板载 CH340 串口不会变成 USB 手柄或音频设备。

当前 Windows 实测设备包括：

- `USB Composite Device`：`USB\VID_054C&PID_0CE6\M61DS5COMPOSITE1`
- `HID-compliant game controller`：`HID\VID_054C&PID_0CE6&MI_03...`
- `DualSense Wireless Controller` 音频控制/媒体设备
- `扬声器 (DualSense Wireless Controller)`
- `耳机式麦克风 (DualSense Wireless Controller)`

`ds5 status` 中的 USB 状态是主要证据：

```text
usb_gamepad ready=<0|1> configured=<0|1> busy=<0|1> sent=<n> dropped=<n>
usb_audio open=<n> close=<n> out_open=<0|1> in_open=<0|1> ...
hidp_reports parsed=<n> full=<n> mic_audio=<n> log=<normal|quiet>
```

- `configured=1` 证明 PC 完成 USB 配置。
- `sent>0` 或持续增长证明输入报文正在写入 USB interrupt endpoint。
- `configured=0` 且电脑只显示 CH340，说明需要接 BL618 原生 USB D+/D-。

## 剩余风险

- 当前开发板可能没有把 Type-C/USB 口接到 BL618 原生 USB，需要焊接或找测试点。
- 串口日志刷屏时，先发 `ds5 log quiet` 再发 `ds5 status`。
- 还需要 PC 端手柄测试工具确认映射和 5 分钟稳定性。
