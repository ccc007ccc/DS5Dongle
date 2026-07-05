# DualSense 移植进度清单

更新时间：2026-07-06

目标不是把 M61 做成普通 HID 手柄，而是尽量复刻 `awalol/DS5Dongle`
的行为：真实 DualSense 通过 Classic Bluetooth HIDP 连接 M61，M61 在 USB
侧枚举成有线 `DualSense Wireless Controller`，并把输入、触摸板、IMU、
自适应扳机、LED、震动、扬声器、3.5mm 耳机、麦克风等 DualSense 特性桥接过去。

状态标记：

- `完成`：代码路径已经存在，并有基本实机或代码证据。
- `部分`：已有一段路径，但不等于完整上游行为，或还没做实机验证。
- `缺失`：当前 M61 主线还没有实现。
- `待验`：需要插板、Windows 枚举或手柄功能测试确认。

## 上游基线

参考仓库：`https://github.com/awalol/DS5Dongle`

本次核对的本地上游副本：`fa83c77`

上游 USB 设备形态：

- VID/PID：`054C:0CE6`
- 产品字符串：`DualSense Wireless Controller`
- USB 不是单 HID，而是复合设备：
  - Interface 0：USB Audio Control
  - Interface 1：USB Audio Streaming OUT，4ch 16-bit 48 kHz，EP `0x01`
  - Interface 2：USB Audio Streaming IN，2ch 16-bit 48 kHz，EP `0x82`
  - Interface 3：DualSense HID，321 字节 report descriptor，IN EP `0x84`，OUT EP `0x03`
  - 可选：wake/gamebar 用 HID keyboard interface

上游关键数据路径：

- BT 输入 `0x31` 直接转 USB HID input report `0x01`，长度 63 字节。
- USB HID output report `0x02` 是 `SetStateData`，包含震动、自适应扳机、LED、音频控制、静音、播放器灯等字段。
- USB Audio OUT 的 4 声道 PCM：
  - ch0/ch1：手柄扬声器或 3.5mm 耳机输出，Opus 编码后进 BT report `0x36`
  - ch2/ch3：HD haptics/高级震动，48 kHz 下采样到 3 kHz 后进 BT report `0x36`
- BT 输入 `0x31` 中带 mic payload 标志时，后续 Opus 麦克风数据解码后写入 USB Audio IN。
- Windows 把 DualSense 的震动/触觉通道当成音频设备的一部分，这是正常行为，不是多余设备。

## 当前 M61 总结

- `完成` M61 Classic Bluetooth HIDP 已连上真实 DualSense，并能收到 `report=0x31 mode=full`。
- `完成` M61 原生 USB 硬件已由 MSC/HID probe 证明可枚举。
- `完成` 2026-07-06 重测确认：补好掉线后，MSC probe 和 DS5 复合固件都能被 Windows 正常枚举；此前“未知 USB 设备/没反应”主要是线路/复位流程问题。
- `部分` 当前主固件 USB 代码已改到 Sony `054C:0CE6`、`DualSense Wireless Controller` 和上游 4-interface 复合布局。
- `完成` Windows 已实测枚举出 USB Composite、HID game controller、DualSense 扬声器和麦克风。
- `部分` USB Audio Control/OUT/IN 已注册，Audio OUT 已取 ch2/ch3 生成 haptics block，Audio IN 仍先送静音包。
- `部分` HID report descriptor 已是 321 字节 DualSense 描述符，HID interface/endpoint 已改为 interface 3、IN `0x84`、OUT `0x03`。
- `部分` BT `0x31` full report 的 63 字节 payload 当前会 raw pass-through 到 USB report `0x01`。
- `部分` USB HID output/feature 有转发雏形，`SetStateData` 已加入状态缓存和 flag 选择性合并；HD haptics-only `0x36` 已实现，待刷写实机验证。
- `完成` 复合 USB 固件已构建、刷写成功；手动 RST/EN normal boot 后 Windows 枚举成功，`ds5 status` 显示 `configured=1`。
- `部分` BT `0x36` 已有 haptics-only 路径；Opus、扬声器、3.5mm、麦克风音频功能链路还没有移植。

## M61/CherryUSB 可用基础

- 本地 SDK 已有 CherryUSB device audio 类：`components/usb/cherryusb/class/audio/usbd_audio.c`。
- 本地 SDK 已有 UAC1 麦克风/扬声器多通道模板：`examples/cherryusb/cherryusb_cli/device_demo/uac_v1_mic_speaker_multichan_template.c`。
- 当前 `m61/dualsense_hidp_probe/defconfig` 已启用 `CONFIG_CHERRYUSB_DEVICE_HID` 和 `CONFIG_CHERRYUSB_DEVICE_AUDIO`。
- 当前 `m61/dualsense_hidp_probe/usb_config.h` 配置了 `CONFIG_USB_MUSB_EP_NUM 8` 和 `CONFIG_USB_MUSB_PIPE_NUM 8`，端点数量理论上够先做 Audio OUT、Audio IN、HID IN、HID OUT。

## USB 枚举清单

| 项目 | 上游 DS5Dongle | 当前 M61 | 状态 | 下一步 |
|---|---|---|---|---|
| VID/PID | `054C:0CE6` | 已改为 `054C:0CE6` | 完成 | 已由 Windows PnP 确认 |
| 产品字符串 | `DualSense Wireless Controller` | 已改为同名 | 完成 | 每次描述符结构变化时修改 serial，避开 Windows 缓存 |
| 设备类型 | USB Composite + Audio + HID | 代码已改为复合设备 | 完成 | Windows 看到 USB Composite Device |
| Audio Control | interface 0 | 已注册 UAC1 interface 0 | 完成 | Windows 看到 `DualSense Wireless Controller` MEDIA 设备 |
| Audio OUT | interface 1, EP `0x01`, iso adaptive, 392 bytes | 已注册；ch2/ch3 下采样为 haptics block | 部分 | 验证 `usb_audio out_pkts`、`usb_haptics queued/peak` 增长 |
| Audio IN | interface 2, EP `0x82`, iso async, 196 bytes | 已注册并发送静音包 | 部分 | 验证 host 打开后 `usb_audio in_pkts` 增长 |
| HID interface | interface 3 | 已改为 interface 3 | 完成 | Windows 看到 `HID-compliant game controller` / `MI_03` |
| HID IN endpoint | `0x84` | 已改为 `0x84` | 部分 | 验证输入 report |
| HID OUT endpoint | `0x03` | 已改为 `0x03` | 完成 | `ds5 status` 看到 `usb_ds5 out=4 last_out=0x02/48` |
| HID report descriptor | 321 bytes DS descriptor | 已有 321 bytes | 完成 | 保持和上游字节级一致 |

当前 Windows 实测 PnP 节点：

| Class | FriendlyName | InstanceId 关键字段 | 状态 |
|---|---|---|---|
| USB | `USB Composite Device` | `USB\VID_054C&PID_0CE6\M61DS5COMPOSITE1` | 完成 |
| HIDClass | `HID-compliant game controller` | `HID\VID_054C&PID_0CE6&MI_03...` | 完成 |
| HIDClass | `USB 输入设备` | `USB\VID_054C&PID_0CE6&MI_03...` | 完成 |
| MEDIA | `DualSense Wireless Controller` | `USB\VID_054C&PID_0CE6&MI_00...` | 完成 |
| AudioEndpoint | `扬声器 (DualSense Wireless Controller)` | `SWD\MMDEVAPI\{0.0.0...` | 完成 |
| AudioEndpoint | `耳机式麦克风 (DualSense Wireless Controller)` | `SWD\MMDEVAPI\{0.0.1...` | 完成 |

## 输入特性清单

| 特性 | 上游路径 | 当前 M61 | 状态 | 下一步 |
|---|---|---|---|---|
| 左右摇杆 | BT `0x31` -> USB `0x01` | 已解析并发送 | 完成 | Windows 手柄面板/SDL 验证 |
| 方向键 | BT `0x31` -> USB `0x01` | 已解析并发送 | 完成 | 验证 8 方向和 idle |
| 基础按键 | BT `0x31` -> USB `0x01` | 已解析并发送 | 完成 | 验证 square/cross/circle/triangle/L1/R1/L3/R3 |
| L2/R2 模拟量 | BT `0x31` -> USB `0x01` | 已解析并发送 | 完成 | 验证 0-255 连续值 |
| PS/Create/Options | BT `0x31` -> USB `0x01` | 已解析并发送 | 部分 | 验证 PS 键行为 |
| Mute 按键 | BT `0x31` -> USB `0x01` | 已解析并发送 | 部分 | 验证 mute 位和灯联动 |
| 触摸板按键 | BT `0x31` -> USB `0x01` | 已解析并发送 | 部分 | 验证 click |
| 触摸板坐标 | payload offset 32 `TouchData` | raw pass-through，但未解析/未测试 | 部分 | 增加 parser 字段和 HID 抓包验证 |
| 陀螺仪 | payload offset 15/17/19 | 已解析，full report raw pass-through | 部分 | 用 Steam/DSX/HID capture 验证 |
| 加速度计 | payload offset 21/23/25 | 已解析，full report raw pass-through | 部分 | 用 Steam/DSX/HID capture 验证 |
| 传感器时间戳/温度 | payload offset 27/31 | 已解析部分字段 | 部分 | 确认 USB report 中保持原值 |
| 电量/供电状态 | payload offset 52 | 已解析并打印 | 部分 | 验证主机侧显示 |
| 3.5mm 插入状态 | payload offset 53 | 已解析 headphones/mic bits | 部分 | 需要接入音频路由和实测 |
| 外置 mic 标志 | payload offset 54 | 未解析 | 缺失 | 扩展 parser |
| 自适应扳机状态反馈 | payload offset 41/42/47 | raw pass-through，未解析 | 部分 | 扩展 parser 并验证触发器状态回读 |

## 输出和手柄控制清单

| 特性 | 上游路径 | 当前 M61 | 状态 | 下一步 |
|---|---|---|---|---|
| HID output report `0x02` | Host -> USB OUT -> `SetStateData` | 已能接收并转 BT `0x31` | 部分 | 验证 report id/长度/CRC/时序 |
| 自适应扳机 | `RightTriggerFFB[11]` / `LeftTriggerFFB[11]` | 随 `0x02` payload 透传 | 部分 | 做结构化 `state_mgr`，用 DSX/Steam 验证 |
| 普通 rumble emulation | `RumbleEmulationRight/Left` | 随 `0x02` payload 透传 | 部分 | 验证无 Audio 时是否立刻生效 |
| HD haptics | USB Audio OUT ch2/ch3 -> BT `0x36` | 已实现 haptics-only：按上游 WDL_Resampler 配置等价实现 48 kHz -> 3 kHz、64B block、`0x36` CRC | 部分 | 刷写后用 Steam/测试工具验证体感和 `hidp_haptics sent` |
| RGB lightbar | `LedRed/Green/Blue` | 随 `0x02` payload 透传 | 部分 | 做 state cache，验证颜色 |
| Player LEDs | `PlayerLight1..5` | 随 `0x02` payload 透传 | 部分 | 验证白色玩家灯 |
| Mute light | `MuteLightMode` | 随 `0x02` payload 透传 | 部分 | 验证 mute 灯模式 |
| Light brightness/fade | `LightBrightness` / `LightFadeAnimation` | 随 `0x02` payload 透传 | 部分 | 验证亮度和动画 |
| 音量/静音控制 | USB Audio control + `SetStateData` | HID payload 可透传，USB Audio control 缺失 | 缺失 | 实现 UAC mute/volume 回调 |
| Feature GET | Host GET_REPORT -> BT feature get -> cache | 已有转发/cache | 部分 | 验证 calibration/firmware/hardware info |
| Feature SET | Host SET_REPORT feature -> BT control with CRC | 已有雏形 | 部分 | 验证 `0x80`、Edge profile 相关不会误处理 |
| 输出状态合并 | 上游 `state_mgr` 维护最新状态 | 已有 63-byte state cache 和 47-byte USB `0x02` flag 合并 | 部分 | 实机验证 LED/rumble/trigger 后再补配置持久化 |

## 音频和触觉清单

| 特性 | 上游路径 | 当前 M61 | 状态 | 下一步 |
|---|---|---|---|---|
| Windows 音频设备枚举 | USB Audio Control/Streaming | 手动 RST/EN normal boot 后已验证 | 完成 | 第一阶段只做枚举，不接 Opus |
| 主机到手柄扬声器 | USB Audio OUT ch0/ch1 -> Opus -> BT `0x36` block `0x13` | 无 | 缺失 | 评估 BL616 CPU/RAM，再移植 Opus encode |
| 主机到 3.5mm 耳机 | USB Audio OUT ch0/ch1 -> Opus -> BT `0x36` block `0x16` | 无 | 缺失 | 根据 headphones bit 切换 block id |
| HD haptics/高级震动 | USB Audio OUT ch2/ch3 -> 48 kHz to 3 kHz -> int8 -> BT `0x36` | 已实现 haptics-only 初版 | 部分 | 当前按上游 `SetMode(true,0,false)`、`SetRates(48000,3000)` 的整数 16:1 input-driven linear path 做 C 等价实现，保留跨包相位和一帧 lookahead |
| 手柄 mic 到电脑 | BT mic payload -> Opus decode -> USB Audio IN | 当前识别后跳过 | 缺失 | 实现 mic queue、Opus decode、USB IN |
| mic active 控制 | Host 打开 Audio IN alternate setting 后启用 | 已记录 `audio_in_open`，未接 BT mic | 部分 | 接入 report `0x32/0x36` mic enable |
| 音量和 mute | UAC SET_CUR/GET_CUR -> `SetStateData` | 已记录 UAC 音量/静音，未接 `SetStateData` | 部分 | 参考上游 `usb.cpp` 更新 state cache |

## 蓝牙私有协议清单

| 项目 | 当前 M61 | 状态 | 下一步 |
|---|---|---|---|
| BR/EDR inquiry/连接 | 已实现 | 完成 | 保持 |
| L2CAP HID control `0x11` | 已实现 | 完成 | 保持 |
| L2CAP HID interrupt `0x13` | 已实现 | 完成 | 保持 |
| Set protocol report | 已实现 | 部分 | 验证错误码和重试 |
| Feature reports `0x09/0x20/0x22/0x05/0x70` | 已请求 | 部分 | 保存并回给 USB host |
| BT output `0x31` | 已构造并带 CRC | 部分 | 接入完整 `state_mgr` |
| BT output `0x32` | 已构造并带 CRC | 部分 | 用于 mic/audio 状态时需扩展 |
| BT audio/haptics `0x36` | 已有 haptics-only framing、sequence、CRC、block `0x12`，HD haptics 转换已按上游 WDL 配置做等价实现 | 部分 | 扬声器/耳机 block `0x13/0x16` 仍缺失 |
| CRC32 seed `0xA2` | 已实现 | 完成 | 保持测试 |
| Feature CRC seed `0x53` | 已实现 | 完成 | 保持测试 |

## 验证清单

### 枚举验证

- `完成` 刷写后按 RST/EN 正常启动，不手动 `ds5 usb-reinit`，Windows 能枚举。
- `完成` Windows 看到 `USB Composite Device`，VID/PID 为 `054C:0CE6`。
- `完成` Windows 看到 `HID-compliant game controller`。
- `完成` Windows 看到 DualSense 相关音频输出设备。
- `完成` Windows 看到 DualSense 相关麦克风输入设备。

推荐检查命令：

```powershell
Get-PnpDevice -PresentOnly |
  Where-Object { $_.InstanceId -match 'VID_054C|PID_0CE6|M61|DS5' -or $_.FriendlyName -match 'DualSense|Wireless Controller|Audio|Microphone|Speaker' } |
  Select Status,Class,FriendlyName,InstanceId |
  Format-List
```

Windows 桌面测试工具：

```powershell
python tools\ds5_windows_test_app.py
```

- `PnP`：确认 `VID_054C&PID_0CE6` 的 USB Composite、HID、Audio 节点。
- `Input report 0x01`：实时显示摇杆、按键、L2/R2、触摸板、IMU、电量、耳机/麦克风状态。
- `Output report 0x02`：发送 LED、普通 rumble、自适应扳机字段，也支持 47/48 字节 raw hex。
- `HD Haptics Tone`：可选依赖 `sounddevice`，向 DualSense 音频输出发 4ch/48kHz 测试波形，只驱动 ch2/ch3。
- `ds5 status`：通过 COM5 读取固件状态，核对 `configured`、`sent`、`usb_ds5 out`、`usb_audio`。
- `Status After HD`：核对 `usb_haptics queued/sample_pairs/nonzero/peak/ds/mode/gain_q8` 和 `hidp_haptics sent/errors/noconn`；`mode=2` 表示 WDL 配置等价路径。

### 手柄功能验证

- `待验` Windows/SDL/Steam 输入：摇杆、方向键、基础按键。
- `待验` L2/R2 模拟量连续变化。
- `待验` 触摸板点击和坐标。
- `待验` 陀螺仪和加速度计。
- `完成` 普通 HID rumble emulation：测试程序可触发；Steam HD 震动不走此路径。
- `待验` 自适应扳机。
- `待验` RGB lightbar、玩家灯、mute 灯。
- `缺失` 手柄扬声器播放。
- `缺失` 3.5mm 耳机输出。
- `缺失` 手柄麦克风录音：当前只有 USB 麦克风枚举和 IN 静音包，未做 BT mic Opus decode。
- `待验` 5 分钟连接稳定性、丢包、延迟。

## 推荐实现顺序

1. 先把 USB 描述符改成上游复合设备，并只做枚举，不做音频处理。
2. 启用 CherryUSB Audio，注册 Audio Control/OUT/IN 三个 interface 和 ISO endpoints。
3. 把 HID interface/endpoint 改到上游编号：interface 3、IN `0x84`、OUT `0x03`。
4. 修改 serial 字符串，清 Windows 旧缓存影响。
5. 验证 Windows 同时出现手柄、音频输出、麦克风输入。
6. 验证 `SetStateData` 状态缓存：自适应扳机、LED、玩家灯、普通 rumble。
7. `部分` 先实现 haptics-only：USB Audio OUT ch2/ch3 -> BT `0x36`，HD haptics 转换已改为上游 WDL 配置等价路径，待刷写验证。
8. 再评估并实现扬声器/3.5mm 的 Opus encode。
9. 最后实现 mic Opus decode -> USB Audio IN。
