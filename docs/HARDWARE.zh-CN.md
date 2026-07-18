# Ai-M61-32s-Kit硬件与接线

[English](HARDWARE.md)

## 目标开发板

正式固件面向Ai-M61-32s-Kit，使用Bouffalo SDK的`CHIP=bl616`目标构建。开发板和
模组资料中可能同时出现BL616、BL618或Ai-M61-32S字样；本项目发布的BL616镜像、
仓库锁定的板级配置以及开发板实际排针图是权威依据。

不要把其他Ai-M61模组或不同开发板当作可直接替换目标。更换板型后必须重新验证Flash
布局、RF参数、USB引脚、时钟和内存。

## 原生USB排针

Ai-M61-32s-Kit已经把原生USB信号直接引出为独立排针，不需要使用GPIO编号：

| USB线信号 | 开发板排针 |
| --- | --- |
| D+ | `USB_DP` |
| D- | `USB_DM` |
| 电源 | `5V` |
| 地 | `GND` |

![Ai-M61-32s-Kit原生USB接线](assets/m61-usb-wiring.png)

开发板自带Type-C口连接CH340串口桥，负责UART刷写、日志和供电，但不能枚举为USB手柄。
电脑必须连接独立的`USB_DP`和`USB_DM`，才能看到：

- USB Composite Device；
- HID-compliant game controller，`VID_054C&PID_0CE6`；
- DualSense扬声器/耳机输出；
- DualSense麦克风输入。

严禁把CH340的USB D+/D-与开发板原生`USB_DP`/`USB_DM`硬并联。

## 供电方式

正常使用时，推荐只连接原生USB线的四根信号：`5V`、`GND`、`USB_DP`、`USB_DM`。
这一根USB线同时给开发板供电并传输手柄数据，不需要再插Type-C/CH340线。

刷入固件时，推荐拔掉原生USB，只保留Type-C/CH340刷写线。若必须保留原生USB的数据线，
至少断开它的5V，确保只有一路5V电源。不要让Type-C/CH340和原生USB同时向开发板供电。

5V只能接开发板标注的`5V`引脚。不要向`3V3`输入5V。USB转杜邦线的颜色不一定遵循
常见红/白/绿/黑定义，接线前应核对线序。

## 普通用户推荐顺序

1. 暂时不要连接原生USB线。
2. 只用Type-C数据线连接开发板，通过CH340进入BOOT+RESET刷写模式并刷入Release固件。
3. 刷写成功后拔掉Type-C线。
4. 按接线图连接`5V`、`GND`、`USB_DP`、`USB_DM`。
5. 把原生USB插入电脑，开发板应正常供电并枚举为DualSense复合设备。
6. 首次配对时让手柄进入创建键+PS键配对模式；已保存的手柄会自动重连。

图形化刷写和配对步骤见[快速入门](QUICK_START.zh-CN.md)。

## 开发和诊断

需要串口日志时，可以让Type-C/CH340和原生USB数据同时连接，但必须断开原生USB的5V，
只保留`USB_DP`、`USB_DM`和`GND`，避免双路供电。

常用验证命令：

```powershell
python tools/check_m61_usb_windows.py
python tools/validate_m61_usb_hardware.py -p COM5
```

只看到COM口/CH340，说明电脑尚未连接原生USB。看到USB Composite Device但没有HID或
音频子设备，通常是D+/D-接反、线材问题或主机枚举失败。

## 状态灯

Ai-M61-32s-Kit默认映射为红GPIO12、绿GPIO14、蓝GPIO15、active high。正常策略为绿色
空闲、连接中蓝灯闪烁、DualSense HIDP就绪后蓝灯常亮。板型差异应覆盖配置宏，不要修改
运行时逻辑。

## 内存与时钟

锁定release保留160 KiB WRAM，应用RAM链接区域为319 KiB，这是实测布局。使用干净SDK
却不带项目级`CONFIG_WRAM_LENGTH=163840`会改变内存布局，不能视为同性能构建。

运行时CPU档位为320、384、400 MHz，默认manual 320 MHz。400 MHz以上属于实验值，稳定性
取决于板子，不能保存为常驻profile。PBCLK和外设稳定性必须与CPU执行分别验证。

## 安全

- 改接USB数据线或电源引脚前先断电。
- 刷写时只保留一路5V供电。
- 不要向`3V3`输入5V。
- 保留UART ISP恢复路径。
- 精确板卡版本的厂商PDF引脚图具有最高硬件权威。
- 本项目未做商业或安全关键用途认证。
