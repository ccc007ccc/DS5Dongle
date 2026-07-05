# M61 ESP32 fallback 调试桥

本文档描述 fallback 工具：让 M61/Ai-M61-32S-Kit 作为串口桥和 GPIO 下载控制器，替代手按 ESP32 `BOOT/IO0` + `EN/RST`。

当前默认主线不是这个桥，而是：

```text
DualSense --Classic Bluetooth HIDP--> M61 --USB DualSense composite--> PC/主机
```

只有当 M61-only 路线被硬件证据否定时，才回到 ESP32 fallback。

## 角色边界

- M61 ESP32 调试桥只用于刷写/复位 ESP32。
- 它不负责当前产品的 USB DualSense composite 输出。
- 当前 M61 HIDP+USB 固件在 `m61/dualsense_hidp_probe`。
- 当前 Ai-M61-32S-Kit 实测可用主机口是板载 CH340 暴露的 M61 UART0，Windows 上通常为 `COM5 @ 115200`。
- M61 原生 USB 要走 BL618 `USB_DP`/`USB_DM`，这和 CH340 串口桥是两条不同硬件链路。

## 推荐接线

开发期优先两块板子各自 USB 供电，只接信号线和共地：

| M61 信号 | ESP32 信号 | 用途 |
| --- | --- | --- |
| `GND` | `GND` | 必须共地 |
| `GPIO23` / UART1 TX | `U0RXD` / `GPIO3` | M61 发给 ESP32 ROM loader |
| `GPIO24` / UART1 RX | `U0TXD` / `GPIO1` | ESP32 ROM loader 回给 M61 |
| `GPIO27` | `BOOT` 键非 GND 侧 / `IO0` / `GPIO0` | 拉低进入下载模式 |
| `GPIO28` | `EN` / `RST` | 复位 ESP32 |

M61 侧 GPIO 不要求叫 `GPIO0`。这里的 `GPIO0` 指 ESP32 的 `IO0/BOOT` 下载脚。

Ai-M61-32S 模组自己的下载脚是 `GPIO2`：上电或复位瞬间高电平进入烧录模式，低电平正常启动。它只用于刷 M61 自身固件，不是拿来控制 ESP32 的 `IO0/BOOT`。

如果 ESP32 排针没有 `IO0/GPIO0`，找 `BOOT` 按键非 GND 侧焊点，建议串 1k 电阻接 M61 `GPIO27`。`EN/RST` 也建议串 1k 电阻后接 M61 `GPIO28`。

## 供电

优先顺序：

1. 开发期最稳：M61 插 USB，ESP32 也插自己的 USB；只共地，不互接 `5V` 或 `3V3`。
2. M61 单线供电 ESP32：M61 `5V/VBUS` 接 ESP32 `5V/VIN`，此时不要再插 ESP32 自己的 USB，避免反灌。
3. M61 3.3V 供 ESP32：只在确认稳压器余量足够时使用。ESP32 峰值电流高，建议保留 500mA 以上余量。

## ESP32 下载控制时序

```text
BOOT_CTL 拉低
EN_CTL 拉低 100ms
EN_CTL 释放
等待 150ms
BOOT_CTL 释放
```

运行模式复位：

```text
BOOT_CTL 释放
EN_CTL 拉低 100ms
EN_CTL 释放
```

## 构建和刷写桥固件

构建：

```powershell
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/esp32_prog_bridge/build.sh
```

刷写：

```powershell
python tools\flash_m61_firmware.py --app bridge -p COM5 -b 115200 --reboot-isp
```

如果自动进 M61 下载失败，手动：

```text
按住 M61 BOOT
按一下 RESET/RST 并松开
松开 BOOT
```

然后：

```powershell
python tools\flash_m61_firmware.py --app bridge -p COM5 -b 115200 --manual-hint
```

## 电脑侧命令

无损探测：

```powershell
python tools\probe_m61_serial.py -p COM5 --dump
```

控制 ESP32：

```powershell
python tools\m61_esp32_control.py -p COM5 -b 115200 status
python tools\m61_esp32_control.py -p COM5 -b 115200 boot
python tools\m61_esp32_control.py -p COM5 -b 115200 run
python tools\m61_esp32_control.py -p COM5 -b 115200 reset
```

通过 M61 刷 ESP32 fallback stage-1：

```powershell
python tools\flash_stage1_m61.py -p COM5 -b 115200 --backend raw-hidp
```

桥接固件支持：

```text
~m61 boot
~m61 boot-hold
~m61 reset
~m61 run
~m61 reboot-isp
~m61 status
~m61 help
```

其中 `~m61 reboot-isp` 是让 M61 自己进 UART 下载口；`~m61 boot` 和 `~m61 boot-hold` 控制的是外接 ESP32 的 `IO0/BOOT`。

## 调试顺序

1. 只接 `GND`、M61 UART1 TX/RX 到 ESP32 RX0/TX0，两板各自 USB 供电。
2. 烧录 M61 桥接固件，确认 `probe_m61_serial.py` 显示 `kind=bridge`。
3. 手动按 ESP32 `BOOT` + `EN`，用 `flash_stage1_manual.py` 验证串口链路。
4. 再接 M61 `GPIO27` 到 ESP32 BOOT，`GPIO28` 到 ESP32 EN/RST。
5. 用 `m61_esp32_control.py boot` 验证自动进下载。
6. 用 `flash_stage1_m61.py` 替代手动 BOOT 烧录。

如果第 3 步失败，先不要接 BOOT/EN，优先检查 UART 交叉、共地、ESP32 UART0 引脚是否接反。
