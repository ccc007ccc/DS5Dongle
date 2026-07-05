# ESP32 fallback stage-1 validation

本文档只用于 ESP32 fallback 路线。当前默认主线是 M61-only：

```text
DualSense --Classic Bluetooth HIDP--> M61 --USB DualSense composite--> PC/主机
```

除非 M61 Classic HIDP 或 BL618 原生 USB 被硬件证据否定，否则不要把新工作切回 ESP32 stage-1。

## fallback 目的

验证 ESP32-WROOM-32 能使用 ESP-IDF Classic Bluetooth HID Host 连接真实 DualSense，并通过串口打印输入状态。这个路线的最终用途是：

```text
DualSense --Classic BT HIDP--> ESP32 --UART2--> M61 --USB DualSense composite--> PC/主机
```

注意：

- ESP32-WROOM-32 没有原生 USB Device/OTG，不能作为 USB 手柄输出端。
- ESP32 使用 ESP-IDF，不使用 Arduino。
- 旧附件里的 ESP32 stage-1 顺序是 fallback 标准，不是当前 M61-only 主线门槛。

## 构建

```powershell
python tools\build_esp32_stage1.py
python tools\build_esp32_stage1.py --backend raw-hidp
```

或在 ESP-IDF shell 中：

```powershell
idf.py set-target esp32
idf.py build
```

## 刷写

自动下载，依赖开发板 DTR/RTS：

```powershell
python tools\flash_stage1_auto.py -p COM5
```

手动 BOOT/EN：

```powershell
python tools\flash_stage1_manual.py -p COM5
```

如果已经刷入 M61 ESP32 调试桥：

```powershell
python tools\flash_stage1_m61.py -p COM5 -b 115200 --backend raw-hidp
```

M61 调试桥使用 ESP32 UART0 刷写，不要和 fallback 产品链路里的 ESP32 UART2 混淆。

## 连接和日志

首次连接时让 DualSense 进入配对模式：长按 `PS + Create/Share`。成功后 ESP32 会尝试保存地址，后续可按 `PS` 唤醒直连。

期望日志包含：

```text
Scanning for DualSense over Classic Bluetooth BR/EDR
Selected candidate ...
HID open status=0 ...
DualSense bring-up attempt ...
report=0x31 mode=full ... LX=... buttons=... gyro=... accel=...
```

采集并检查：

```powershell
python tools\capture_stage1_log.py -p COM5 --duration 300 --reset -o stage1.log
python tools\check_stage1_log.py stage1.log --min-reports 20 --min-duration-ms 300000 --require-output-init --require-full-report --require-input-activity
```

## fallback 通过标准

- ESP32 能完成配对和 HID 连接。
- 日志出现完整 `report=0x31 mode=full`。
- 摇杆、按键、L2/R2、IMU、电量字段和实操对应。
- 连接稳定 5 分钟，无持续断连、明显延迟或持续丢包。
- 首次成功连接后能优先尝试保存的 DualSense 地址，不要求每次进入配对模式。

## 已知风险

- ESP-IDF HID Host 是否能完整暴露 DualSense 私有报文和控制通道，需要实机验证。
- 如果默认 HID Host 后端卡在 `report=0x01 mode=basic`，可切换 raw Classic L2CAP HIDP backend。
- 该路线不会替代 M61 原生 USB；USB 输出仍必须走 BL618 原生 `USB_DP`/`USB_DM`。
