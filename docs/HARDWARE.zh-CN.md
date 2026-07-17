# 硬件与接线

[English](HARDWARE.md)

## 目标身份

本固件针对Ai-M61-32S系列，使用Bouffalo SDK的`CHIP=bl616`目标构建。厂商材料会同时
使用BL616/BL618系列术语；对本仓库而言，编译目标和生成的BL616镜像格式是权威。更换
board definition前必须重新验证USB引脚、RF校准、Flash布局、时钟和内存。

## 必须使用原生USB

SoC USB Device引脚：

| 信号 | 固件引脚 |
| --- | --- |
| USB D+ | GPIO32 / `USB_DP` |
| USB D- | GPIO33 / `USB_DM` |
| Ground | GND |

很多Ai-M61-32S-Kit的USB-C口连接CH340串口桥。该接口仍用于供电、日志和UART刷写，但
固件无法把CH340变成手柄。

电脑必须通过第二路原生USB看到：

- USB Composite Device；
- HID-compliant game controller，`VID_054C&PID_0CE6`；
- DualSense speaker/headset输出；
- DualSense microphone输入。

严禁把CH340 D+/D-与SoC D+/D-硬并联。

## 供电

如果开发板已通过CH340接口供电，第二条原生USB线只连接D+、D-和GND，避免两个未受控
5 V源。

如果原生USB同时供电，必须接开发板文档标注的`5V`/`VBUS`/`VIN`入口。不要向模组
`VCC`直接输入5 V；模组VCC通常是3.3 V。

## 推荐bring-up顺序

1. 保留CH340连接用于串口日志和刷写。
2. 把原生D+、D-、GND接到可靠USB转接板/线。
3. 构建并刷入锁定release固件。
4. 松开BOOT后RESET进入正常启动。
5. 运行`python tools/check_m61_usb_windows.py`。
6. 配对/连接手柄，再运行
   `python tools/validate_m61_usb_hardware.py -p COM5`。

只看到COM5/CH340说明原生USB没有连接或未配置。看到USB composite但没有HID/audio子项，
通常是描述符或主机驱动枚举失败。

## 状态灯

默认Ai-M61-32S-Kit映射为红GPIO12、绿GPIO14、蓝GPIO15、active high。正常策略为绿色
空闲、连接中蓝灯闪烁、DualSense HIDP就绪后蓝灯常亮。板型差异应覆盖配置宏，不要修改
运行时逻辑。

## 内存与时钟

锁定release保留160 KiB WRAM，应用RAM链接区域为319 KiB，这是实测布局。使用干净SDK
却不带项目级`CONFIG_WRAM_LENGTH=163840`会改变内存布局，不能视为同性能构建。

运行时CPU档位为320、384、400 MHz，默认manual 320 MHz。400 MHz以上属于实验值，稳定性
取决于板子，不能保存为常驻profile。PBCLK和外设稳定性必须与CPU执行分别验证。

## 安全

- 改接USB数据线或电源引脚前先断电。
- 不要把超频镜像作为默认release发布。
- 保留UART ISP恢复路径。
- 精确板卡版本的厂商PDF引脚图具有最高硬件权威。
- 本项目未做商业或安全关键用途认证。
