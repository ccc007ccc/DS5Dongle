# M61 Windows 一键刷写器

这是供普通用户使用的单文件 Windows 图形化刷写工具。双击正式EXE只显示GUI，不需要
命令行。最终 EXE 内置 Bouffalo
`BLFlashCommand.exe`，启动后读取主仓库的固件 Release 列表，由用户选择要下载安装的
版本。用户电脑不需要 Python、Rust、SDK 或编译器。

运行时会：

1. 校验内置刷写核心的 SHA256；
2. 从 GitHub Releases 读取具有`M61-Firmware-<版本>.zip`的固件列表；
3. 下载用户选择的ZIP，先校验GitHub SHA256 digest，再校验ZIP内三件套和校验清单；
4. 自动检测 `USB\VID_1A86&PID_7523` 的 CH340 和 COM 口；
5. 检测到硬件但没有可用 COM 口时，从 WCH 官方 HTTPS 地址下载驱动；
6. 校验驱动的 Authenticode 签名者后，请求 UAC 打开官方安装器；
7. 引导用户用 BOOT+RESET 进入可靠的 UART ISP 模式；
8. 默认以 460800 baud 刷写，失败时可用 115200 baud 重试。

GUI包含固件下拉列表、CH340状态、串口与速度选择、ISP步骤确认、实时刷写日志以及失败
重试窗口。首次启动按Windows用户区域语言自动选择简体中文或English，右上角可随时切换。
操作日志支持鼠标选择和“复制全部”，ISP风险提示使用红色。

## 在线与本地固件

- `在线 Release`：工具只下载一个完整固件ZIP，再校验、解包和刷写；
- `本地固件 ZIP`：选择从Release下载或自行制作的同格式ZIP，适合离线安装；
- `本地目录（高级）`：选择同时包含boot2、`partition.bin`和应用BIN的目录。

ZIP内允许三件套位于根目录或一个子目录。工具会拒绝路径穿越、重复三件套、错误partition
大小、过大文件和不匹配的SHA256清单。正式Release ZIP必须带校验清单。

构建时会从SDK纳入Git的`eflash_loader_cfg.conf`确定性生成运行时`.ini`，不依赖刷写工具
上次运行后留下且被SDK忽略的状态文件，因此干净CI和本地构建结果一致。

## 本地构建

构建时把环境变量指向锁定SDK内的`BLFlashCommand.exe`。构建脚本会同时从它旁边读取
`chips/bl616`所需的loader配置和flash参数：

```powershell
$env:M61_BLFLASHCOMMAND = 'C:\path\to\BLFlashCommand.exe'
cargo build --manifest-path tools\m61-flasher\Cargo.toml --release
```

这些SDK文件只嵌入最终EXE，不会加入Git。固件始终在运行时从Release列表下载，或由用户
选择本地ZIP/目录。

## 开发检查

```powershell
cargo test --manifest-path tools\m61-flasher\Cargo.toml
cargo build --manifest-path tools\m61-flasher\Cargo.toml
tools\m61-flasher\target\debug\m61-flasher.exe --assets-info
tools\m61-flasher\target\debug\m61-flasher.exe --list-releases
tools\m61-flasher\target\debug\m61-flasher.exe --release v0.8.1 --verify-release --yes
tools\m61-flasher\target\debug\m61-flasher.exe --release v0.8.1 --tool-preflight --yes
tools\m61-flasher\target\debug\m61-flasher.exe --list
tools\m61-flasher\target\debug\m61-flasher.exe --dry-run
```

命令行后端只用于开发测试；正式`--release`构建使用Windows GUI subsystem，双击不会出现
命令行窗口。
