# M61 DualSense适配器

[English](README.md)

这是一个面向Ai-M61-32S（BL616/BL618系列）的开源单芯片DualSense转USB项目。
M61通过Bluetooth Classic HIDP连接真实DualSense，再通过原生USB向电脑枚举为
`054C:0CE6` DualSense复合设备。

```text
DualSense -- Bluetooth Classic HIDP --> M61 -- 原生USB --> PC
```

这是独立社区项目，与Sony Interactive Entertainment没有隶属或背书关系。
“DualSense”是其相应权利人的商标。

## 当前功能

| 范围 | 已实现 |
| --- | --- |
| 蓝牙 | BR/EDR扫描、配对/安全、SDP、HID控制/中断L2CAP、已保存手柄自动重连 |
| USB HID | DualSense描述符与输入报告、输出报告、完整Feature GET/SET代理 |
| 手柄输出 | 灯带/玩家灯、静音灯、普通震动、自适应扳机 |
| 音频与触觉 | 48 kHz/16-bit四通道USB OUT、扬声器/耳机路由、HD haptics、Opus传输 |
| 麦克风 | DualSense Opus解码为48 kHz/16-bit双声道USB IN（复制手柄单声道） |
| 运行时控制 | 麦克风开关、单/双声道/自动扬声器路由、可持久化manual/realtime DVFS |
| 诊断 | 串口状态/bring-up命令、编译期开关控制的HPM/pipeline/runtime profile、主机验证工具 |

正式固件默认值保守且确定：CPU为320 MHz manual模式、麦克风关闭、扬声器自动路由、
不启用编译期超频。完整限制和实测状态见[功能矩阵](docs/FEATURES.zh-CN.md)。

可通过M61专用WebHID配置器修改并持久化这些设置：
<https://ds5.766677.xyz/>。请用Chromium内核浏览器通过HTTPS
打开，连接M61，先读取当前配置，再把完整配置保存到Flash。USB回报模式包括实时转发
新报告，以及硬件实测通过的固定250 Hz和500 Hz；固定模式可能重复最近的蓝牙样本，
不会提高手柄本身的原始采样率。

## 使用Release预编译固件

不需要自行编译时，可从[Releases](https://github.com/ccc007ccc/DS5Dongle/releases)
中选择列出完整刷写文件的版本，并下载同一版本的以下文件：

- `boot2_bl616_isp_release_v8.1.8.bin`；
- `partition.bin`；
- `m61_dualsense_hidp_probe_bl616.bin`；
- 对应的`flash-files.sha256`校验文件。

把三个BIN放到`m61/dualsense_hidp_probe/build-win/build_out/`，核对SHA256后即可使用
后文的`--windows-build`刷写命令。刷写工具仍需要把锁定的`bl_mcu_sdk`克隆到项目同级
目录，但不需要安装工具链或构建Opus。不要混用不同Release的boot2、partition和应用BIN。

## 性能可复现构建

默认构建就是经过实测的release性能配置，不会静默回退到SDK低性能实现：

- Bouffalo SDK `2.3.24`固定到提交
  `d9306a4a221db414131337ec95113e3adaf7072b`；
- Xuantie/T-Head GCC 10.2.0 V2.6.1固定到对应平台提交；
- Opus 1.2.1按SHA256下载，并应用仓库内11个E907优化补丁后从源码构建；
- `m61_usb_gamepad.c`使用`-O2`，Opus使用`-O2 -flto`，WRAM固定160 KiB，
  codec窗口1 ms，启用Flash nibble CRC和decode-MDCT SRAM放置；
- 正式构建关闭HPM、pipeline、任务runtime和Opus stage诊断。

`build_windows.ps1`和`build.sh`默认拒绝未锁定的SDK或工具链。每次成功构建都会在
固件旁生成JSON来源清单。完整锁文件为
[`reproducible-build.lock.json`](m61/dualsense_hidp_probe/reproducible-build.lock.json)。

## Windows快速开始

按锁定提交克隆三个仓库：

```powershell
git clone https://github.com/ccc007ccc/DS5Dongle.git
git clone https://github.com/bouffalolab/bl_mcu_sdk.git
git -C bl_mcu_sdk checkout d9306a4a221db414131337ec95113e3adaf7072b
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_windows.git
git -C toolchain_gcc_t-head_windows checkout 072fc29d765774d66366c57a4d962e90c366ef1b

cd DS5Dongle\m61\dualsense_hidp_probe
.\build_windows.ps1 -Command All `
  -SdkPath C:\path\to\bl_mcu_sdk `
  -ToolchainBin C:\path\to\toolchain_gcc_t-head_windows\bin
```

脚本会自动下载并校验Opus。release产物位于：

```text
m61/dualsense_hidp_probe/build-win/build_out/
  m61_dualsense_hidp_probe_bl616.bin
  m61_dualsense_hidp_probe_bl616.elf
  m61_dualsense_hidp_probe_bl616.map
  m61_dualsense_hidp_probe_bl616.manifest.json
```

覆盖任何release参数前先阅读[构建与刷写](docs/BUILDING.zh-CN.md)。参数被覆盖后，
清单会明确标记为`custom`，不能宣称与正式性能构建等价。

## 硬件

电脑必须接到BL616/BL618原生`USB_DP`和`USB_DM`。很多Ai-M61开发板的USB口只连接
CH340串口，不能让固件枚举成手柄。请按[硬件与接线](docs/HARDWARE.zh-CN.md)操作；
严禁把CH340的USB数据线与SoC原生USB引脚硬并联。

## 刷写与验证

进入UART下载模式后，在仓库根目录运行：

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 --windows-build
python tools\check_m61_usb_windows.py
python tools\validate_m61_usb_hardware.py -p COM5
```

如果运行中的固件支持`ds5 reboot-isp`，刷写工具可加`--reboot-isp`自动请求ISP；
否则按住BOOT、点按RESET，再松开BOOT。

## 文档

- [功能与当前缺口](docs/FEATURES.zh-CN.md)
- [架构](docs/ARCHITECTURE.zh-CN.md)
- [构建与刷写](docs/BUILDING.zh-CN.md)
- [硬件与接线](docs/HARDWARE.zh-CN.md)
- [协议与音频格式](docs/PROTOCOL.zh-CN.md)
- [性能与基准规则](docs/PERFORMANCE.zh-CN.md)
- [开发与验证](docs/DEVELOPMENT.zh-CN.md)
- [依赖、许可证与再分发](docs/OPEN_SOURCE.zh-CN.md)

## 许可证

项目自有源码和文档按[MIT License](LICENSE)开源。外部SDK与Opus继续使用各自许可证，
详见[第三方声明](THIRD_PARTY_NOTICES.md)。
