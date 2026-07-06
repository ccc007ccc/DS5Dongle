# M61 + ESP32-WROOM-32 Dual-Chip Performance Target

本文档只讨论性能最优目标，不讨论实现难度和阶段拆分。目标是在保留 M61 原生 USB 能力的前提下，用 ESP32-WROOM-32 的双核和 Classic Bluetooth 能力隔离蓝牙实时调度压力，让 M61 的单核主要服务 USB、DualSense 协议状态和音频编解码。

## 结论

目标架构：

- M61/BL616：USB 复合设备、USB Audio/HID 实时端点、DualSense 私有协议状态机、USB -> DS5 报文生成、HD haptics 下采样、speaker Opus encode、mic Opus decode。
- ESP32-WROOM-32：Classic Bluetooth HIDP Host、L2CAP control/interrupt channel、手柄连接/重连/鉴权/SDP、BT 发送队列调度、BT 输入和 mic Opus 接收转发。
- 双芯片通信：SPI + IRQ GPIO + RESET GPIO。M61 做 SPI master，ESP32 做 SPI slave/DMA。UART 只作为调试日志，不作为最终实时数据链路。

核心原则：

- 不跨芯片传 USB 原始 48 kHz 音频流。
- M61 生成完整 DS5 BT output report，ESP32 只负责按时发给手柄。
- ESP32 收到 DS5 input/mic/audio 状态后原样或轻解析转发给 M61，M61 维护对 Windows 暴露的 USB 状态。
- 实时队列以 deadline 和 latest-wins 为主，不为音频/震动包做重传。

这个分工的性能理由：

- ESP32-WROOM-32 没有 USB，不能替代 M61 做 Windows 侧 DualSense USB composite。
- M61 性能更适合 Opus 和 USB Audio 同步，但单核同时跑 BT stack、USB、Opus、日志和状态机会挤占 10 ms 音频预算。
- ESP32 双核和原生 Classic BT 适合承接 BT Host/transport 这种并发多、单次计算小、但调度敏感的任务。
- 跨芯片只传 DS5 层报文时，数据量约几十 KB/s，SPI 很宽裕；传原始 USB Audio OUT 会变成数百 KB/s，并且把 1 ms USB 抖动传播到第二颗芯片。

## 当前代码分支状态

分支 `m61-esp32-dual-chip` 已开始按本目标拆分代码边界：

- 共享 SPI frame 协议已落到 `main/dual_chip_spi_proto.*`，包含固定 header、CRC、消息类型、channel、priority、deadline、`HELLO` 能力握手、`TIME_SYNC` payload、ACK payload、`STATS` 快照和 `0x36` 实时 report 识别；`tools/test_dualsense_protocol.py` 已加入 SPI frame、`HELLO`、`TIME_SYNC`、`FLOW_CREDIT`、ACK、`STATS` 与 `RESET_STATS` 离线向量。
- M61 侧新增 `m61/dualsense_hidp_probe/m61_esp32_transport.*`，现有 USB/Opus/DS5 output report 生成点可以在 `CONFIG_M61_DS5_DUAL_CHIP_TRANSPORT=y` 时走 ESP32 transport，而不是 M61 本机 HIDP 发送。
- M61 侧已有可配置 SPI master shell：`CONFIG_M61_ESP32_SPI_ENABLE` 和 `CONFIG_M61_ESP32_SPI_READY` 默认关闭，`SCLK/MOSI/MISO/CS/READY/IRQ/RESET` 默认 `255` 作为无效 pin，不碰硬件；填入实际接线并显式打开后会用固定 532B MTU 与 ESP32 slave 做同步 exchange。
- M61 SPI exchange 已能验证 ESP32 返回 frame，并把 `BT_RX_INPUT` 回灌到 USB HID input、把 `BT_RX_MIC_OPUS` 回灌到 USB microphone Opus 队列。
- M61 侧已有默认关闭的 RX poll shell：`CONFIG_M61_ESP32_RX_POLL_ENABLE=n`；接好 `ESP_IRQ` 并显式打开后会在 IRQ 高电平时 clock 空事务读取 ESP32 pending response；如果 `ESP_IRQ` 暂未接线但需要先 bring-up，也可以只打开该选项，让 M61 按固定间隔 fallback polling。
- 双芯片 feature report 控制面已分离为 `BT_TX_FEATURE_GET`、`BT_TX_FEATURE_SET`、`BT_RX_FEATURE_REPORT`，ESP32 走 HIDP control channel，M61 回填 USB feature cache。
- `BT_CONNECT/BT_DISCONNECT` 可靠控制面已接入：M61 transport 和 `ds5 connect`/`ds5 disconnect [reconnect]` shell 命令会转发到 ESP32；双芯片 shell 现在区分 `ds5 autoconnect`（保存地址后再 fallback 扫描）、`ds5 scan`（强制扫描）和 `ds5 connect last`（只尝试保存地址）；ESP32 raw HIDP 也可按 6 字节 BDA 指定目标；如果显式 BDA connect 发生在 ESP32 `L2CAP/SDP` 尚未 ready 之前，该 target 会保留到 stack ready 后继续执行，不会被保存地址/扫描 fallback 覆盖；如果手动指定地址后的重连定时器触发，ESP32 也会保留这次手动 target，而不是退回保存地址/扫描路径。`BT_DISCONNECT` 会清掉 pending target/discovery/SDP 状态，避免迟到的 completion 把连接又偷偷拉起。
- `BT_FORGET` 可靠控制面已接入：M61 `ds5 forget` 会转发到 ESP32，清掉 ESP32 侧保存的 DualSense 地址并删除 Classic BT bond database，避免双芯片模式只清掉 M61 本地缓存、但 ESP32 仍继续按旧地址/旧 link key 自动连接；同时也会清掉 pending target/discovery/SDP 状态，避免 forget 之后被旧事件继续推进连接。
- ESP32 raw HIDP 现在会把“保存 DualSense 地址”和“成功显式重配后移除黑名单”的 NVS 持久化延后到连接稳定窗口之后处理，而不是在 `L2CAP open` 热路径里同步刷 flash，减少刚连上时的阻塞风险。
- 可靠控制面的 ACK 与超时重传已接入：M61 对 `HELLO`、feature/reset 这类 reliable frame 带 `DS5_DUAL_FLAG_RELIABLE`，ESP32 收到后用同 type + `DS5_DUAL_FLAG_ACK` 回传原 seq/type/status；M61 发送 reliable frame 后会 clock 空事务取 ACK，ACK miss 或可恢复错误会按 `CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT` 重发，并在 transport stats 记录 ACK/miss/retry/fail/status。
- `HELLO` 握手已接入：ESP32 SPI slave ready 后预置本机 role/capability/MTU/queue-depth，M61 初始化 SPI transport 时发可靠 `HELLO`，并在 stats 中记录对端 role、protocol version、MTU、max payload、queue depth 和 capabilities。
- `TIME_SYNC` 已接入一跳同步和周期刷新：M61 初始化时发送本地微秒时间，ESP32 返回 `esp_timer` 微秒时间，M61 估算 offset 后才把实时 `0x36` 的 FreeRTOS tick deadline 转成 ESP32 微秒 deadline；`CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS` 默认 1000 ms，transport ready 后低优先级刷新 offset，同步状态、失败次数、RTT、age 和 offset 会显示在 M61 transport stats 中。
- `BT_STATE` 基础回传已接入：ESP32 raw HIDP 在 L2CAP/SDP/open/close/bring-up/full-report 等状态变化时上报 flags、last error、RSSI、MTU、目标地址和状态序号；M61 transport stats 会打印最近一次协处理器蓝牙状态。
- ESP32 已能通过 `FLOW_CREDIT` 回报 BT TX 队列余量、raw HIDP ready、SPI response drop 和 HIDP TX error 计数；M61 侧在 transport stats 中记录最近一次 credit，并会对 `DROP_OK` 的最新实时包做主动限流。
- M61 侧已有 reset 恢复状态机：连续通信/同步失败达到 `CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD=8` 且冷却时间满足后，会在有效 `CONFIG_M61_ESP32_RESET_PIN` 上拉低 50 ms、等待 750 ms boot，再重新 `HELLO` + `TIME_SYNC`；当前左侧接线 profile 仍保持 `RESET_PIN=255`，因此默认不会实际拉任何 GPIO。
- ESP32 侧新增 `main/esp32_dual_chip_spi.*`，用于接收 M61 的完整 BT report frame、latest-wins 入队，并复用 raw HIDP backend 发送到 DualSense；ESP32 -> M61 response 使用小型环形队列，ACK 插到队头，避免可靠控制帧被旧状态/credit response 阻塞。
- ESP32 侧已有可配置 SPI slave 任务：`CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO/MOSI/MISO/CS_GPIO` 默认 `-1` 不碰硬件；填入实际接线后会初始化 `spi_slave_transmit()` 固定 MTU 事务，并可用 `ESP_READY/ESP_IRQ` GPIO 暴露 ready 和 pending response。
- ESP32 raw HIDP 暴露 `bt_dualsense_raw_hidp_send_report()` 和 RX callback，方便双芯片 transport 只负责 L2CAP/HIDP 传输与输入回传统计。
- `STATS` 诊断快照已接通：M61 `ds5 status` 会向 ESP32 请求 SPI/HIDP/deadline/ACK/connect 计数，ESP32 返回低优先级快照，不进入实时音频通道。
- 双芯片 bring-up 日志检查已接入：`tools/check_dual_chip_log.py` 可离线校验 M61 `ds5 status` 输出，`tools/validate_dual_chip_hardware.py` 可通过 M61 串口周期性采集 `ds5 status` 并自动检查 SPI 握手、`TIME_SYNC`、ACK、`STATS`、`FLOW_CREDIT` 和 `BT_STATE`。
- `sdkconfig.dual_chip.defaults` 是 ESP32 双芯片 coprocessor 配置入口；M61 侧目前默认不启用双芯片 transport，避免在 SPI/IRQ/RESET 针脚未确定前刷入不可用固件。
- 首个建议接线 profile 已记录在 `docs/DUAL_CHIP_WIRING.md`：M61 使用左侧 `IO13/IO11/IO10/IO20` 对 ESP32 左侧 `GPIO27/GPIO26/GPIO25/GPIO33`，`READY/IRQ` 使用 M61 左侧 `IO16/IO17` 对 ESP32 左侧 `GPIO32/GPIO13`；对应 ESP32 defaults 为 `sdkconfig.dual_chip.devkit_left.defaults`，M61 示例片段为 `m61/dualsense_hidp_probe/defconfig.dual_chip_left_spi.example`，M61 构建脚本支持 `--profile dual-chip-left-spi` 显式构建该配置。旧 `devkit-vspi` profile 保留为右侧排针备选。

尚未完成：

- 真实板级接线仍未完成，因此 M61/ESP32 两侧 SPI 硬件配置默认关闭，尚未做上板联调。
- 真实 reset 线尚未分配和接线；如果 `TIME_SYNC` 没成功，M61 会继续发送零 deadline，避免不同 tick 基准导致 ESP32 误丢实时包。

## 性能边界

当前 DualSense 相关实时流量：

- USB Audio OUT：48 kHz, 4 channel, 16-bit，原始速率约 384 KB/s。当前 USB iso packet 为 392 B/ms。
- speaker Opus：200 B / 10 ms，约 20 KB/s。
- HD haptics block：64 B / 10 ms，约 6.4 KB/s。
- BT report `0x36`：398 B / 10 ms，约 39.8 KB/s。
- BT output `0x31`：78 B，状态类，低频或合并发送。
- BT output `0x32`：142 B，mic/audio status，低频或状态变化发送。
- BT input `0x31`：手柄输入，按手柄上报频率转发，必须低延迟。
- mic Opus：手柄到主机方向，ESP32 接收后转给 M61，由 M61 解码成 USB Audio IN。

目标性能指标：

- USB HID input 端到端延迟：目标 < 4 ms，最大 < 8 ms。
- USB output 到 BT `0x31/0x32` 状态生效：目标 < 10 ms。
- USB Audio OUT 到 BT `0x36` 发送：稳定 100 Hz，deadline jitter 目标 < 1 ms。
- speaker Opus encode：M61 上平均 < 8 ms，P95 < 9 ms，最大不超过 10 ms；持续播放时 `qdrop=0` 为目标。
- BT `0x36` 发送错误：持续 60 s 播放时 `errors=0`，无 L2CAP buffer starvation。
- SPI 实时通道占用：目标 < 20% bus utilization，避免芯片间链路成为新瓶颈。

## M61 目标职责

M61 是 USB 主控和协议真源。

必须保留在 M61：

- USB device 枚举和描述符：
  - HID interface
  - USB Audio speaker endpoint
  - USB Audio microphone endpoint
  - feature/output report cache
- Windows 侧 DualSense 行为：
  - HID input report `0x01`
  - HID output report `0x02`
  - feature report get/set
  - Audio OUT/IN open/close/mute/volume/frequency control
- DualSense 私有 output report 生成：
  - BT `0x31` SetStateData
  - BT `0x32` audio/mic status
  - BT `0x36` haptics/speaker/headset audio
  - CRC、sequence、audio packet counter、audio buffer length
- 音频和触觉计算：
  - USB Audio OUT ch0/ch1 -> speaker Opus
  - USB Audio OUT ch2/ch3 -> HD haptics 3 kHz block
  - BT mic Opus -> USB Audio IN PCM
- USB 与 BT 状态融合：
  - LED/player light/mute light
  - adaptive trigger state
  - rumble/haptics/audio control
  - mic active 策略

M61 不应该承担：

- Classic BT discovery/pairing/reconnect 的主流程。
- HIDP L2CAP control/interrupt channel 的实时收发。
- BT buffer 分配/重试/连接恢复调度。
- 手柄蓝牙链路的周期性保活和连接状态细节。

M61 调度目标：

- USB ISR/callback 只做拷贝和轻量状态更新，不做 BT 发送。
- Opus encode 任务独立于 BT stack，不被 L2CAP 发送阻塞。
- `0x36` 生成使用最新 haptics block 和最新 speaker Opus，错过 deadline 时丢旧包，不追历史包。
- mic decode 低于 speaker encode 优先级；speaker 活跃时允许暂停或降频 mic decode，保证播放和 haptics。

## ESP32-WROOM-32 目标职责

ESP32 是蓝牙实时协处理器。

必须放到 ESP32：

- Classic Bluetooth controller/host。
- DualSense BR/EDR 连接、鉴权、SDP。
- HIDP control channel 和 interrupt channel。
- 自动重连、断线恢复、连接状态机。
- BT report 发送节奏控制。
- BT 输入接收、时间戳、去抖/队列化。
- BT mic Opus payload 接收和转发。
- BT 发送错误统计、L2CAP buffer pressure 统计。

ESP32 不应该做：

- USB 描述符或 USB 状态。
- Windows HID/Audio 语义。
- DualSense feature/output 状态合并的最终决策。
- speaker Opus encode。
- mic Opus decode。
- HD haptics 下采样。

ESP32 双核目标分配：

- Core 0：ESP-IDF BT controller/host、L2CAP/HIDP、BT event loop。
- Core 1：SPI slave DMA、跨芯片队列、deadline scheduler、状态统计。

ESP32 对 DS5 报文的理解应保持最小：

- 识别 report id：`0x31`、`0x32`、`0x36`。
- 识别通道类型：control / interrupt。
- 识别 deadline 和 priority。
- 不重建 DualSense output payload，不修改 payload 字段，除非后续明确需要由 ESP32 做 CRC/sequence。

## 双芯片物理链路

目标链路：

- SPI：M61 master，ESP32 slave，DMA。
- SPI clock：目标 10-20 MHz，保底不低于 8 MHz。
- ESP_IRQ：ESP32 -> M61，表示 ESP32 有输入、状态或 credit 更新。
- ESP_READY：ESP32 -> M61，表示 ESP32 boot 完成且 SPI slave ready；可与 IRQ 合并但不推荐。
- ESP_RESET：M61 -> ESP32，用于硬复位蓝牙协处理器。
- UART：可选，只用于调试日志，不参与实时协议。

为什么 M61 做 SPI master：

- M61 是 USB 设备端，USB OUT 和 USB IN 的时序由 M61 承接。
- M61 可以在 USB 1 ms tick 或 audio bridge tick 后主动发起 SPI transaction。
- ESP32 有 BT 输入时通过 IRQ 拉起 M61 读取，不需要 ESP32 抢占总线。
- M61 能统一控制跨芯片通信对 USB/Opus 任务的影响。

## SPI 协议目标

每个 SPI frame 使用固定 header + payload：

```text
magic      2 bytes  固定同步字
version    1 byte
type       1 byte   消息类型
flags      2 bytes  reliable/latest/drop-ok/ack-needed 等
channel    1 byte   ctrl/input/output/audio/status/log
priority   1 byte   rt/audio/hid/control
seq        2 bytes  每 channel 独立递增
deadline   4 bytes  以同步 tick 表示，0 表示无 deadline
length     2 bytes  payload 长度
crc32      4 bytes  header 不含 crc + payload
payload    N bytes
```

目标消息类型：

- `HELLO`：版本、能力、最大 payload、SPI MTU、队列深度。
- `TIME_SYNC`：M61 tick 与 ESP32 tick 对齐，用于 deadline 判断。
- `BT_CONNECT`：连接控制。payload 为空表示按保存地址/扫描自动连接；payload 为 1 字节时表示模式选择（`SCAN_ONLY` 或 `SAVED_ONLY`）；payload 为 6 字节时表示连接指定 DualSense 地址。
- `BT_DISCONNECT`：主动断开，并清空 pending target/discovery/SDP 状态；可选择是否允许自动重连。
- `BT_FORGET`：清空保存地址和 bond database，并清空 pending target/discovery/SDP 状态。
- `BT_STATE`：连接、鉴权、SDP、HIDP channel、错误码、RSSI。
- `BT_RX_INPUT`：ESP32 -> M61，手柄 input report。
- `BT_RX_MIC_OPUS`：ESP32 -> M61，手柄 mic Opus payload。
- `BT_TX_REPORT`：M61 -> ESP32，完整 BT output report。
- `BT_TX_AUDIO_RT`：M61 -> ESP32，完整 `0x36` report，实时、drop-old。
- `BT_TX_FEATURE_GET`：M61 -> ESP32，HIDP feature get request。
- `BT_TX_FEATURE_SET`：M61 -> ESP32，HIDP feature set request。
- `BT_RX_FEATURE_REPORT`：ESP32 -> M61，HIDP feature report response。
- `FLOW_CREDIT`：ESP32 -> M61，BT TX queue credit 和 L2CAP buffer 状态。
- `STATS`：双向统计计数，用于诊断。
- `RESET_STATS`：清零统计。

可靠性策略：

- control 消息可靠 ACK，失败重发。
- feature/status 类消息可靠或 latest-wins，取决于语义。
- input report 不重传，保留最新若干个，过期丢弃。
- `0x36` audio/haptics 不重传，过 deadline 直接丢弃。
- BT output `0x31` 状态可以合并，保留最新 state；trigger/LED 这类状态不追历史。

## 队列和优先级

M61 -> ESP32：

1. `BT_TX_AUDIO_RT`：最高优先级，deadline 10 ms，队列深度 2，满时丢旧。
2. `BT_TX_FEATURE_GET/BT_TX_FEATURE_SET`：可靠控制，高优先级。
3. `BT_TX_REPORT` control/interrupt：高优先级，队列深度 4，状态类 latest-wins。
4. `BT_CONNECT/BT_DISCONNECT`：可靠控制，队列深度 4。
5. `STATS/LOG`：最低优先级，不能影响实时通道。

ESP32 -> M61：

1. `BT_RX_INPUT`：最高优先级，带时间戳，队列深度按 4-8 个输入报告，过期丢旧。
2. `BT_RX_MIC_OPUS`：高优先级，但低于 input；speaker 活跃时允许降级。
3. `BT_RX_FEATURE_REPORT`：可靠控制，高优先级。
4. `BT_STATE/FLOW_CREDIT`：高优先级，状态变化即时上报。
5. `STATS/LOG`：最低优先级。

关键策略：

- 实时音频和触觉使用 latest-wins。
- 不允许低优先级日志占用 SPI 或 BT TX 时间片。
- ESP32 如果发现 BT TX credit 不足，应立刻通过 `FLOW_CREDIT` 告诉 M61 降低或合并发送，而不是在 ESP32 内部堆积过期音频包。

## 音频与触觉数据流

目标 speaker/haptics 路径：

```text
Windows USB Audio OUT
    -> M61 USB iso callback
    -> M61 ch0/ch1 speaker frame
    -> M61 Opus encode
    -> M61 ch2/ch3 HD haptics downsample
    -> M61 make DS5 BT 0x36
    -> SPI BT_TX_AUDIO_RT
    -> ESP32 HIDP interrupt send
    -> DualSense
```

目标 mic 路径：

```text
DualSense mic Opus
    -> ESP32 HIDP receive
    -> SPI BT_RX_MIC_OPUS
    -> M61 Opus decode
    -> M61 USB Audio IN
    -> Windows
```

目标 input 路径：

```text
DualSense input 0x31
    -> ESP32 HIDP receive
    -> SPI BT_RX_INPUT
    -> M61 USB HID input 0x01
    -> Windows
```

目标 output/control 路径：

```text
Windows HID output/feature
    -> M61 state cache and DS5 output merge
    -> M61 make BT 0x31/0x32/0x36
    -> SPI BT_TX_REPORT / BT_TX_AUDIO_RT
    -> ESP32 HIDP send
    -> DualSense
```

## 不采用的性能目标

不把 USB Audio OUT 原始 PCM 整流送到 ESP32：

- 4 channel 48 kHz 16-bit 是约 384 KB/s。
- 这会把 USB 1 ms packet 抖动传到跨芯片链路和 ESP32 调度。
- ESP32 还要同时跑 BT controller/host，原始音频流会压缩 BT 实时余量。
- M61 已经必须接 USB，保留音频聚帧和 encode 更符合数据局部性。

不把 DualSense 私有协议状态分散到两颗芯片：

- LED、trigger、audio control、mute、player light、haptics、speaker、mic active 都互相关联。
- 状态拆散后会增加跨芯片一致性问题。
- 性能上更好的边界是 M61 生成完整 BT payload，ESP32 只传输。

不让 ESP32 修改 `0x36` payload：

- `0x36` 包含 sequence、audio packet counter、haptics block、speaker/headset block、CRC。
- ESP32 修改 payload 会让协议状态跨芯片同步变复杂。
- ESP32 只需要知道 deadline，不需要知道音频内容。

## 诊断指标

M61 必须保留或新增：

- USB HID sent/dropped/busy。
- USB Audio OUT/IN packet count。
- speaker frames/encoded/qdrop/odrop/qdepth/oqdepth。
- Opus encode last/avg/max。
- haptics queued/nonzero/qdrop/peak。
- mic opus/decoded/underflow/nonzero。
- SPI TX/RX bytes、frame errors、crc errors、deadline miss。
- M61 -> ESP32 realtime queue drop-old count。

ESP32 必须输出：

- BT connected/security/sdp/hidp channel state。
- HIDP TX report count by id：`0x31/0x32/0x36`。
- HIDP TX errors、last_err、L2CAP credit/buffer pressure。
- HIDP RX input count、mic Opus count。
- BT reconnect count、disconnect reason。
- SPI RX/TX frame count、crc errors、queue drops。
- `0x36` deadline miss count。

联合判定目标：

- 如果 M61 `enc_us` 仍超过 10 ms，瓶颈是 M61 Opus encode 或 USB/Opus 调度。
- 如果 M61 `enc_us` 达标但 ESP32 `0x36 deadline miss` 增长，瓶颈是 SPI/ESP32/BT transport。
- 如果 ESP32 `0x36 sent` 稳定但声音仍卡，瓶颈是 DS5 `0x36` payload 格式、音频 block/cadence 或 Opus 参数。
- 如果 BT errors 或 credit pressure 增长，瓶颈是 Classic BT L2CAP buffering 或 ESP32 BT stack 调度。

## 最终完成标准

目标系统应满足：

- Windows 一侧枚举为完整 DualSense USB composite，包括 HID、扬声器、麦克风。
- USB 只在 DualSense 手柄 full report 已稳定后注册，避免空设备或错误设备。
- 摇杆、按键、触摸板、IMU、电量输入稳定转发。
- LED、player light、mute light、普通震动、自适应扳机可由 Windows/Steam/DSX 控制。
- HD haptics 通过 USB Audio ch2/ch3 转 BT `0x36`，长时间无明显丢节奏。
- 手柄扬声器通过 USB Audio ch0/ch1 -> Opus -> BT `0x36`，60 s 连续播放无可感知断续。
- 手柄麦克风通过 BT mic Opus -> USB Audio IN，有稳定非零输入。
- 打开 Steam/DSX 时不死机、不掉线、不让 shell 长时间无响应。
- 断开手柄后可自动重连上一次设备。
- 任一芯片 reset 后系统可恢复到可连接状态，不需要重新刷写。
