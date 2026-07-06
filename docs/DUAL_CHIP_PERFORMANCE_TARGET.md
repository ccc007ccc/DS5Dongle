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
- `BT_CONNECT`：连接指定 DualSense 地址。
- `BT_DISCONNECT`：主动断开。
- `BT_STATE`：连接、鉴权、SDP、HIDP channel、错误码、RSSI。
- `BT_RX_INPUT`：ESP32 -> M61，手柄 input report。
- `BT_RX_MIC_OPUS`：ESP32 -> M61，手柄 mic Opus payload。
- `BT_TX_REPORT`：M61 -> ESP32，完整 BT output report。
- `BT_TX_AUDIO_RT`：M61 -> ESP32，完整 `0x36` report，实时、drop-old。
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
2. `BT_TX_REPORT` control/interrupt：高优先级，队列深度 4，状态类 latest-wins。
3. `BT_CONNECT/BT_DISCONNECT`：可靠控制，队列深度 4。
4. `STATS/LOG`：最低优先级，不能影响实时通道。

ESP32 -> M61：

1. `BT_RX_INPUT`：最高优先级，带时间戳，队列深度按 4-8 个输入报告，过期丢旧。
2. `BT_RX_MIC_OPUS`：高优先级，但低于 input；speaker 活跃时允许降级。
3. `BT_STATE/FLOW_CREDIT`：高优先级，状态变化即时上报。
4. `STATS/LOG`：最低优先级。

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

