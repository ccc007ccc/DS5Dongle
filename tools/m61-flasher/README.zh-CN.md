# M61 Windows 一键刷写器

这是供普通用户使用的单文件 Windows 图形化刷写工具。双击正式EXE只显示GUI，不需要
命令行。最终 EXE 内置 Bouffalo
`BLFlashCommand.exe`，启动后读取主仓库的固件 Release 列表，由用户选择要下载安装的
版本。用户电脑不需要 Python、Rust、SDK 或编译器。

运行时会：

1. 校验内置刷写核心的 SHA256；
2. 从 GitHub Releases 读取具有完整 boot2、partition 和应用 BIN 的固件列表；
3. 下载用户选择的版本，并按 GitHub 提供的 SHA256 digest 校验每个文件；
4. 自动检测 `USB\VID_1A86&PID_7523` 的 CH340 和 COM 口；
5. 检测到硬件但没有可用 COM 口时，从 WCH 官方 HTTPS 地址下载驱动；
6. 校验驱动的 Authenticode 签名者后，请求 UAC 打开官方安装器；
7. 引导用户用 BOOT+RESET 进入可靠的 UART ISP 模式；
8. 默认以 460800 baud 刷写，失败时可用 115200 baud 重试。

GUI包含固件下拉列表、CH340状态、串口与速度选择、ISP步骤确认、实时刷写日志以及失败
重试窗口。首次启动按Windows用户区域语言自动选择简体中文或English，右上角可随时切换。

## 本地构建

构建时只需要锁定 SDK 内的 `BLFlashCommand.exe`：

```powershell
$env:M61_BLFLASHCOMMAND = 'C:\path\to\BLFlashCommand.exe'
cargo build --manifest-path tools\m61-flasher\Cargo.toml --release
```

构建脚本只读取该工具并嵌入 EXE，不会把二进制加入 Git。固件始终在运行时从 Release
列表中选择和下载。

## 开发检查

```powershell
cargo test --manifest-path tools\m61-flasher\Cargo.toml
cargo build --manifest-path tools\m61-flasher\Cargo.toml
tools\m61-flasher\target\debug\m61-flasher.exe --assets-info
tools\m61-flasher\target\debug\m61-flasher.exe --list-releases
tools\m61-flasher\target\debug\m61-flasher.exe --release v0.8.1 --verify-release --yes
tools\m61-flasher\target\debug\m61-flasher.exe --list
tools\m61-flasher\target\debug\m61-flasher.exe --dry-run
```

命令行后端只用于开发测试；正式`--release`构建使用Windows GUI subsystem，双击不会出现
命令行窗口。
