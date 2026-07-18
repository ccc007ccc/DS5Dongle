# 构建与刷写

[English](BUILDING.md)

普通用户不需要搭建本文的构建环境。从项目
[Releases](https://github.com/ccc007ccc/DS5Dongle/releases)下载
`M61-Flasher-Windows.exe`，双击后选择固件版本即可。工具会自动识别CH340、在确实缺少
驱动时下载并验证WCH官方安装器、下载并校验所选Release固件ZIP，并引导BOOT+RESET
刷写。也可以先下载`M61-Firmware-<版本>.zip`，再在GUI中选择“本地固件ZIP”离线刷写。

## 支持的release环境

正式构建由
[`reproducible-build.lock.json`](../m61/dualsense_hidp_probe/reproducible-build.lock.json)
定义，而不是由电脑上碰巧安装的SDK或编译器定义。

| 依赖 | 锁定版本 |
| --- | --- |
| Bouffalo SDK | `2.3.24`，`d9306a4a221db414131337ec95113e3adaf7072b` |
| Windows T-Head工具链 | `072fc29d765774d66366c57a4d962e90c366ef1b` |
| Linux T-Head工具链 | `c4afe91cbd01bf7dce525e0d23b4219c8691e8f0` |
| GCC身份 | Xuantie-900 V2.6.1 B-20220906，GCC 10.2.0 |
| Opus | 1.2.1压缩包SHA256 `cfafd339...70732`及11个锁定补丁 |

还需要Python 3、Git、SDK自带的CMake/Ninja/Make和`tar`。主要实测主机为
Windows 10/11与PowerShell 7。

## 手工刷写Release文件（开发者备用）

不自行构建时，从列出完整刷写文件的同一个
[Release](https://github.com/ccc007ccc/DS5Dongle/releases)
下载boot2、partition、应用BIN和对应的`flash-files.sha256`。三个BIN必须来自同一版本，
并放入：

```text
m61/dualsense_hidp_probe/build-win/build_out/
  boot2_bl616_isp_release_v8.1.8.bin
  partition.bin
  m61_dualsense_hidp_probe_bl616.bin
```

校验SHA256后，按本文“刷写”一节使用`--windows-build`。该流程不需要编译器或Opus，
但仓库同级仍需锁定提交的`bl_mcu_sdk`，因为刷写工具来自SDK。

正式Release还会把上述文件和校验清单打包成单个`M61-Firmware-<版本>.zip`。GUI在线下载
和本地ZIP安装使用同一个安全解析与校验流程；ZIP可把文件放在根目录或同一个子目录，
但必须只有一份boot2、partition和应用BIN。

维护者可复现生成ZIP：

```powershell
python tools\package_m61_firmware_zip.py `
  --input-dir C:\path\to\release-files `
  --tag v0.8.1 `
  --output M61-Firmware-v0.8.1.zip
```

脚本先验证校验清单，再用固定条目顺序、时间戳和权限生成ZIP；相同输入重复运行得到相同
文件。

## Windows release构建

```powershell
git clone https://github.com/ccc007ccc/DS5Dongle.git
git clone https://github.com/bouffalolab/bl_mcu_sdk.git
git -C bl_mcu_sdk checkout d9306a4a221db414131337ec95113e3adaf7072b
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_windows.git
git -C toolchain_gcc_t-head_windows checkout 072fc29d765774d66366c57a4d962e90c366ef1b

cd DS5Dongle\m61\dualsense_hidp_probe
.\build_windows.ps1 -Command All -SdkPath C:\work\bl_mcu_sdk -ToolchainBin C:\work\toolchain_gcc_t-head_windows\bin
```

首次构建会下载并校验Opus压缩包，从全新目录解压，按锁定顺序应用全部补丁，再构建
优化库。不再要求用户手工魔改SDK。

依赖门禁会检查：

1. SDK提交完全一致且SDK工作区干净；
2. 工具链提交、编译器身份以及Windows编译器SHA256完全一致；
3. 每个Opus补丁的SHA256；
4. 产物清单中的release配置。

## Linux release构建

```bash
git clone https://github.com/ccc007ccc/DS5Dongle.git
git clone https://github.com/bouffalolab/bl_mcu_sdk.git
git -C bl_mcu_sdk checkout d9306a4a221db414131337ec95113e3adaf7072b
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git
git -C toolchain_gcc_t-head_linux checkout c4afe91cbd01bf7dce525e0d23b4219c8691e8f0

cd DS5Dongle/m61/dualsense_hidp_probe
BL_SDK_BASE=/work/bl_mcu_sdk M61_TOOLCHAIN_BIN=/work/toolchain_gcc_t-head_linux/bin ./build.sh all
```

Windows与Linux之间不要共用GCC LTO归档；两个主机应从相同源码和选项分别构建。

## 锁定的release配置

| 设置 | Release值 |
| --- | --- |
| 目标 | BL616 / `bl616dk` |
| WRAM分区 | 160 KiB |
| USB/codec编译单元 | `-O2` |
| Opus | 定点、`-O2 -flto`、E907补丁配置 |
| Opus放置 | `pvq-mdct-decode-mdct` |
| codec成对服务窗口 | 1 ms |
| CRC | Flash中的16项nibble表 |
| 编译期超频 | 关闭 |
| Profiling | 全部关闭 |
| 运行时默认 | manual 320 MHz、mic关闭、speaker route auto |

`-HpmProfile`、`-PipelineProfile`、`-RuntimeProfile`、
`-OpusStageProfile`或非默认Opus放置都会生成custom构建。这些选项适合测量，但会增加
开销或改变代码布局。

`-AllowUnverifiedDependencies` / `--allow-unverified-dependencies`只供开发。
这种固件不得作为同性能release发布。

## 来源清单

每次成功构建都会在BIN/ELF/MAP旁写入
`m61_dualsense_hidp_probe_bl616.manifest.json`，记录源码与依赖提交、全部性能参数、
锁文件哈希、补丁哈希、实际`SOURCE_DATE_EPOCH`和产物SHA256。发布固件时必须一起
发布清单。

构建脚本默认把`SOURCE_DATE_EPOCH`设为源码提交时间，使SDK中的`__DATE__`/`__TIME__`
板级和驱动字符串保持稳定，不写入本机墙上时间。同一工作区连续两次干净构建必须产生
完全一致的BIN和ELF SHA256；跨机器比较时以清单为权威依据。开发时可以覆盖epoch，
但清单会标记为`custom`。

## 刷写

普通用户运行`M61-Flasher-Windows.exe`，从列表选择固件并按屏幕提示操作即可。工具默认
使用460800 baud，首次失败时会提供115200 baud兼容档重试。

以下命令只用于源码开发者手工刷写。先按住BOOT，点按并松开RESET，再松开BOOT：

```powershell
python tools\flash_m61_firmware.py -p COM5 --windows-build
```

默认460800 baud是正常快速路径。请选择M61的CH340 UART（`1A86:7523`）；Windows
出现多个COM口时不要选择错误状态的端口。只有线材或Hub不稳定时才回退`-b 115200`。

对正在运行的兼容固件可以尝试`--reboot-isp`，但它只是尽力而为的快捷方式。部分BL616
热复位进入BootROM后会读取eFuse失败，因此物理BOOT+RESET仍是唯一可靠的进入方式。
写入后如果仍停留在ISP，松开BOOT并RESET进入正常启动。

## 验证

```powershell
python tools\run_offline_checks.py
python tools\check_m61_realtime_memory.py m61\dualsense_hidp_probe\build-win\build_out\m61_dualsense_hidp_probe_bl616.elf
python tools\check_m61_usb_windows.py
python tools\validate_m61_usb_hardware.py -p COM5
```

串口采集和验证工具只用于开发诊断。测量手柄输入或音频前必须关闭这些程序，也不要在
吞吐或稳定性测试期间保留`ds5 log normal`。

真机性能验收使用[性能文档](PERFORMANCE.zh-CN.md)规定的固定90秒负载。
