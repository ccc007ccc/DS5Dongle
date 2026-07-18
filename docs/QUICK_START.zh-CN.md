# Ai-M61-32s-Kit快速入门

[English](QUICK_START.md)

本文面向只想使用Release固件的Windows用户，不需要安装Python、Rust、SDK或编译器。

## 需要准备

- Ai-M61-32s-Kit开发板；
- 一根Type-C数据线，用于CH340刷写；
- 一根USB公头转四根杜邦线，用于正常使用时的原生USB；
- 普通版DualSense手柄；
- Windows 10/11和Chrome或Edge。

当前没有测试DualSense Edge。固件没有1000 Hz模式；稳定选项为250 Hz、500 Hz和实时跟随。
麦克风默认关闭，开启会显著增加实时负载。

## 第一步：下载刷写器

从项目Releases下载最新版`M61-Flasher-Windows.exe`：

<https://github.com/ccc007ccc/DS5Dongle/releases/latest>

刷写器会列出完整固件版本、检查CH340串口，并在确实缺少驱动时提供WCH官方驱动安装。

![M61图形化刷写器](assets/flasher.png)

## 第二步：刷入固件

刷写阶段只连接Type-C/CH340，不要同时插原生USB线。

1. 按住开发板BOOT键。
2. 点按并松开RESET/RST。
3. 松开BOOT键。
4. 打开刷写器，选择在线Release或本地完整固件ZIP。
5. 选择Ai-M61-32s-Kit的CH340串口，保持推荐的460800速度。
6. 点击“下载并刷写”，等待校验和写入成功。
7. 刷写完成后拔掉Type-C线。

如果460800因线材或Hub不稳定而失败，再使用115200兼容速度重试。

## 第三步：连接原生USB

正常使用时由原生USB一根线完成供电和手柄数据：

- USB D+接开发板`USB_DP`；
- USB D-接开发板`USB_DM`；
- USB GND接开发板`GND`；
- USB 5V接开发板`5V`。

不要向`3V3`输入5V。线材颜色不一定可靠，请先确认D+/D-/5V/GND定义。

![Ai-M61-32s-Kit原生USB接线](assets/m61-usb-wiring.png)

接好后把原生USB插入电脑。正常情况下，Windows会出现DualSense复合设备、游戏手柄和
音频设备，而不是只有一个CH340串口。

## 第四步：连接手柄

首次配对或更换手柄时：

1. 同时按住手柄创建键和PS键，直到灯条快速闪烁。
2. 插入原生USB或按一次开发板RESET。
3. 等待状态灯蓝色常亮。

已经保存的手柄会自动重连；必要时按一次PS键唤醒手柄。网页也提供“配对新手柄”、
“断开手柄”和“忘记手柄”操作。

## 第五步：网页设置

用Chrome或Edge打开：

<https://ds5.766677.xyz/>

1. 点击“连接”，选择Ai-M61-32s-Kit枚举的DualSense设备。
2. 点击“从M61读取”。
3. 修改需要的选项。
4. 点击“保存到Flash”，否则断电后不会保留。

建议先使用发布默认值：320 MHz、麦克风关闭、不超频、死区0%、音频缓冲48。

![M61 WebUI](assets/webui.png)

## 常见问题

### 刷写器找不到串口

- 确认当前连接的是开发板Type-C/CH340口；
- 重新按BOOT+RESET进入ISP；
- 在刷写器中重新检测CH340驱动和串口。

### Windows只有COM口，没有手柄

COM口属于CH340。正常手柄数据必须通过独立的`USB_DP`、`USB_DM`、`GND`和`5V`原生
USB线连接。

### 插入原生USB后没有设备

- 检查D+和D-是否接反；
- 确认USB线确实包含数据线；
- 确认5V接到开发板`5V`而不是`3V3`；
- 断电后重新接线，再按RESET启动。

### 刷写时如何避免双路供电

最简单的方式是拔掉原生USB，只插Type-C/CH340。若必须保留原生USB数据连接，至少断开
它的5V线。

更完整的硬件、电源和开发诊断说明见[硬件与接线](HARDWARE.zh-CN.md)和
[构建与刷写](BUILDING.zh-CN.md)。
