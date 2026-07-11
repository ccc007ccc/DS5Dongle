# M61 单芯片实时系统风险登记

## 1. 范围

本文跟踪 `m61/dualsense_hidp_probe` 单 M61 主线的实时性、音频、触觉、
Bluetooth HIDP、USB 和资源风险。风险编号保持稳定，后续提交应引用编号并更新
状态，避免问题只存在于聊天或临时测试记录中。

状态定义：

- `OPEN`：尚未修复；
- `MITIGATED`：已有保护，但设计不变量尚未完全闭合；
- `CLOSED`：实现、构建门禁和测试均已覆盖；
- `HARDWARE`：代码侧已完成，等待实机长时间验证。

## 2. 当前基线

- 分支：`m61-realtime-scheduler-redesign`
- 基线提交：`2b75943`
- 音频线格式：547 字节 Bluetooth `0x39`
- 生产周期：两个 512 USB frame epoch 组成一包，约 46.875 Hz
- Opus：48 kHz、10 ms、单声道编码器、160 kbps CBR、200 字节
- 实时媒体年龄上限：64 ms
- Bluetooth ACL TX MTU：672 字节
- M61 静态 RAM 门禁：`static + 8 KiB < 75% of 424960 B`
- 当前接入 canonical TX scheduler 后：静态物理 RAM `182764 B`，加 8 KiB
  contingency 为 `190956 B`，距 75% 门限仍有 `127764 B`。

## 3. 待修风险

### M61-RT-001 USB Audio 回调工作量仍然过大

- 严重度：中
- 状态：`MITIGATED`
- 位置：`m61_usb_gamepad.c` 的 `usbd_audio_out_ep_callback()`；
  `m61_audio_epoch.c` 的 `m61_audio_epoch_ingest_usb()`。
- 现状：已改为 16 槽固定 ingress ring；USB endpoint callback 只复制一个最多
  392 字节的 packet；独立高优先级 ingress worker 批量消费并构造 epoch，Opus
  编码期间仍可清空 ingress。packet sequence gap 会原子推进 stream generation、
  清空 backlog 并 reset partial epoch，禁止把缺口两侧 PCM 拼接或发送旧 pending
  pair。尚未完成实机 callback 最大耗时
  和 ingress high-water 验证。
- 风险：约每 1 ms 对 Opus 编码和 Bluetooth host 调度注入抖动；负载升高时可能
  增加 encode P99 和 L2CAP inter-send gap。
- 后续方向：用 task notification 替代 ingress worker 的 1 ms poll，并将当前关
  IRQ 的 packet copy 改为 acquire/release SPSC；实机闭合 callback 耗时、
  ingress residence、high-water 和 overflow 指标。
- 验收：callback 最大耗时可测且低于 100 us；30 分钟混合负载下 encode P99
  `< 10 ms`，无 ingress overflow。

### M61-RT-002 Feature SET 不是可靠 FIFO

- 严重度：中
- 状态：`MITIGATED`
- 位置：USB host report 单 mailbox；`usb_hid_bridge_task()` 的
  `feature_set_pending`。
- 现状：SET_FEATURE 已进入固定深度 8 FIFO；bridge 只取一个 pending transaction，
  K_NO_WAIT 失败时保留并重试，后续事务继续留在 FIFO。
- 风险：连续配置、DSE/profile 或未来控制命令可能静默丢失。
- 后续方向：补充 generation、retry count、deadline 和完成/失败 ACK；FIFO 满时向
  host 暴露明确 admission failure，而不是只增加 drop counter。
- 验收：连续 8 个不同 SET_FEATURE 在持续音频下保持顺序并全部完成；第 9 个明确
  返回 admission failure，不允许静默覆盖。

### M61-RT-003 `0x31/0x32` 在获得 TX buffer 前生成序号和 CRC

- 严重度：中
- 状态：`MITIGATED`
- 位置：`hidp_send_usb_output_report()`、`hidp_send_audio_status_report()`、
  `dualsense_output.c`。
- 现状：运行时 `0x31/0x32` 已先通过统一 helper 取得 net_buf 和 tailroom，再调用
  builder 生成 sequence/CRC；allocation failure 不再改变 output context。bringup
  初始化路径仍保留旧的先构造再发送流程。
- 风险：`-ENOMEM` 重试会人为烧掉序号；未来若加入跨 class 重排，可能出现 wire
  sequence 倒退或状态报告被手柄视为旧包。
- 后续方向：canonical TX scheduler 落地后，让所有 class（包括 bringup/control）
  都由唯一 TX owner 在 selection 后统一 stamp。
- 验收：模拟 class 重排和连续 allocation failure，实际提交的 wire sequence 始终
  单调取模，CRC 与最终序号匹配。

### M61-RT-004 Generation 检查存在 TOCTOU 窗口

- 严重度：中
- 状态：`MITIGATED`
- 位置：`usb_hid_bridge_task()` 对 pending audio pair 的 generation 检查与
  `bt_l2cap_chan_send()` 之间。
- 现状：发送前会检查 generation，但 USB reset/open/close 仍可能恰好发生在检查后。
- 进展：versioned selection、generation reset 和三类 interrupt TX 的 publish/select/
  finish 已全部收敛到唯一 bridge owner；异步 callback 不直接操作 scheduler。发送前
  同时复查 scheduler version 和 USB audio generation。
- 风险：连接或 USB generation 切换时最多发出一个旧媒体包。
- 修复方向：由 canonical TX scheduler 持有 generation；reset 在同一临界区清空
  pending class，发送选择使用 versioned slot 或 generation token。
- 验收：在选择和发送之间注入 reset，旧 generation 的 transmitted 计数必须为 0。

### M61-RT-005 缺少完整 canonical Bluetooth TX scheduler

- 严重度：中
- 状态：`MITIGATED`
- 位置：`usb_hid_bridge_task()`。
- 现状：已实现独立 `m61_bt_tx_scheduler`。RT `0x39` 使用 depth-2、chronological、
  replace-oldest 和 64 ms stale policy；`0x31/0x32` 使用 latest mailbox；最多连续
  3 个 RT 后推进一个 eligible state class；`0x31` 的 20 ms 限速通过 eligibility
  mask 保留，且不会阻塞 RT。`-ENOMEM/-EAGAIN` 保留同 version 重试，fatal error
  显式 drop。每 class 暴露 accepted/transmitted/replaced/stale/retried/dropped/
  rejected/pending，host test 校验 terminal 守恒。
- 风险：interrupt class 已统一，但 control class 和 completion credit 尚未统一；
  后续若绕过 scheduler 直接发送，仍可能重新引入优先级倒置或 buffer 争用。
- 剩余风险：GET/SET feature control 尚未纳入同一 scheduler；发送仍由 1 ms poll
  驱动而不是 completion credit；最终 selection-send 的异步 link/generation 窗口
  仍需硬件和 sent callback 模型闭合。
- 修复方向：把 reliable control FIFO 接入 scheduler，并由 L2CAP sent callback
  归还 credit/唤醒唯一 TX owner。
- 验收：每 class 守恒计数闭合；最多连续 3 个 RT 后 state 有界推进；control 在持续
  音频下完成且不增加 RT stale/drop。

### M61-RT-006 USB input pump 缺少独立通知任务和严格并发模型

- 严重度：中
- 状态：`MITIGATED`
- 位置：`usb_input_pump()`、HID IN completion callback、bridge 周期 poll。
- 现状：已实现 latest mailbox 和 completion pump，提交失败依赖 1 ms bridge poll
  再试。
- 风险：bridge 被其他慢路径占用时 input latency 增加；部分 last payload 读取路径
  尚未统一到同一锁/版本模型。
- 修复方向：固定 input mailbox 加 version；BT RX 只 publish+notify；HID completion
  和专用 realtime dispatcher 触发 pump。
- 验收：500 Hz 输入下 BT RX 到 USB submit P99 `< 4 ms`、最大 `< 8 ms`；USB busy
  时始终提交最新状态。

### M61-RT-007 音频 resampler 不是 band-limited

- 严重度：中
- 状态：`OPEN`
- 位置：`resample_epoch_speaker_mono()`。
- 现状：512 到 480 使用线性插值，并在编码前 L/R 下混。
- 风险：可能产生高频混叠和可测音质损失；当前主要目标是消除卡顿，不代表达到
  主机级最终音质。
- 修复方向：固定系数 polyphase/FIR 或验证过的 WDL 等效 band-limited resampler；
  保持同一 epoch 和 10 ms Opus deadline。
- 验收：频响、THD+N、alias rejection 与参考实现对比；encode P99 仍 `< 10 ms`。

### M61-RT-008 当前为真正 mono 编码，需确认最终声道目标

- 严重度：中
- 状态：`HARDWARE`
- 现状：旧实现虽然初始化 stereo encoder，但已强制 `FORCE_CHANNELS=1`；当前实现
  直接用 mono encoder，降低 CPU，但不会恢复 stereo 空间信息。
- 风险：若 DualSense speaker/headset 路径实际要求 stereo，当前音质目标不完整。
- 修复方向：分别测 mono 160 kbps 和 stereo 160 kbps 的 encode P99、听感、距离
  稳定性和 RAM；只有 stereo 持续满足 deadline 才切换。
- 验收：明确产品声道规范；30 分钟无 encode drop，盲听和声道测试通过。

### M61-RT-009 动态 Bluetooth pool/heap 预算尚未形成运行时门禁

- 严重度：中
- 状态：`OPEN`
- 现状：静态 RAM gate 已闭合；672 字节 ACL pool 会增加运行时 heap，但当前 gate
  只检查 ELF 静态 section。
- 风险：连接、feature、mic 和 audio 同时活动时最小 heap 可能低于预期。
- 修复方向：启动、连接后和混合负载期间记录 current/min free heap；门禁目标
  `min free heap > 100 KiB`，同时记录 task stack HWM。
- 验收：60 分钟 soak 无 allocation failure、stack overflow 或 watchdog reset。

### M61-RT-010 旧 speaker/haptics FIFO 兼容代码仍留在源文件

- 严重度：低
- 状态：`OPEN`
- 现状：运行路径已切换到 epoch，但若干旧函数和静态对象仍以 unused 兼容代码保留。
- 风险：维护者可能误接回旧路径；增加审查噪音和少量潜在 RAM/ROM 占用。
- 修复方向：在硬件验证 epoch 路径后删除旧 queue、take API、diagnostic 字段或完成
  明确迁移，不保留双实现。
- 验收：post-link symbol gate 禁止 legacy queue/processor symbol；RAM gate继续通过。

### M61-RT-011 缺少硬件长时间 RF/音频联合验收

- 严重度：中
- 状态：`HARDWARE`
- 风险：代码修复不能消除真实 RF 丢包；距离变远时仍需区分 radio gap、CPU overload
  和本地排队放大。
- 验收：近距离和目标距离各运行至少 30 分钟 speaker+haptics；记录 RSSI、RT
  accepted/transmitted/stale/failed、encode avg/P99/max 和 L2CAP gap P99/max。

### M61-RT-012 缺少发送完成回调驱动的 credit 模型

- 严重度：中
- 状态：`OPEN`
- 现状：K_NO_WAIT allocation failure 后按 1 ms poll 重试。
- 风险：无 buffer 时产生固定轮询抖动；不能精确区分 controller credit、host pool 和
  radio completion 延迟。
- 修复方向：接入 `bt_l2cap_chan_ops.sent` 或等效 completion，归还 credit 时通知唯一
  TX scheduler；保留低频 watchdog 作为丢通知保护。
- 验收：正常流式传输不依赖 1 ms poll；TX wakeup 和 completion 计数守恒。

## 4. 已关闭高风险和回归约束

### M61-RT-C01 本地 ACL buffer 小于 `0x39`

- 状态：`CLOSED`
- 修复：全局编译 `CONFIG_BT_L2CAP_TX_MTU=672`；编译期断言要求至少 548 字节；
  发送前检查 `net_buf_tailroom()`。
- 回归门槛：预处理后的 `acl_tx_pool` 必须显示 672；禁止只检查协商 MTU。

### M61-RT-C02 RT pair 在 allocation failure 后永久丢失

- 状态：`CLOSED`
- 修复：canonical TX scheduler 的 RT depth-2 slot 持有 pair，`-ENOMEM/-EAGAIN`
  时保留同 version 并在下一轮优先重试；超过 64 ms 才 stale drop。
- 回归门槛：模拟连续 allocation failure 后恢复，发送的必须是仍在年龄上限内的
  pending pair。

### M61-RT-C03 SET_FEATURE 使用 `K_FOREVER` 阻塞媒体

- 状态：`CLOSED`
- 修复：SET_FEATURE 改为 K_NO_WAIT，并保留 pending 重试。
- 回归门槛：所有 USB/BT realtime dispatcher 调用链禁止出现 `K_FOREVER` allocation。

### M61-RT-C04 Speaker/haptics 独立时钟和旧 Opus 重复发送

- 状态：`CLOSED`
- 修复：4 个 typed epoch slot；每 epoch 512 USB frame；16-frame box filter；
  generation+epoch keyed encode completion；仅相邻两个 epoch 组成 `0x39`。
- 回归门槛：host test 必须覆盖相邻配对、gap、generation reset、box average 和容量。

### M61-RT-C05 旧 `0x36` 100 Hz 空口负载

- 状态：`CLOSED`
- 修复：改为 547 字节 `0x39` 双 epoch，约 46.875 Hz；应用 payload bitrate 从约
  318 kbps 降到约 205 kbps。
- 回归门槛：547 字节 offset/CRC 向量测试必须通过。

### M61-RT-C06 USB HID IN busy 时丢失最新输入

- 状态：`CLOSED`
- 修复：latest input mailbox；HID IN completion 立即 pump；周期 realtime task 为
  submit failure 提供重试。
- 回归门槛：端点 busy 期间多次 publish 后只发送最新状态，不等待下一条 BT report。

## 5. 建议修复顺序

1. `M61-RT-001`：USB audio ingress ring，先消除 callback 抖动。
2. `M61-RT-005` + `M61-RT-012`：canonical TX scheduler 和 completion credit。
3. `M61-RT-002` + `M61-RT-003` + `M61-RT-004`：可靠 control、发送时 stamp、
   generation 原子化。
4. `M61-RT-006`：独立 USB input dispatcher。
5. `M61-RT-009`：运行时 heap/stack gate。
6. `M61-RT-010`：删除 legacy path 并增加 symbol gate。
7. `M61-RT-007` + `M61-RT-008`：音质和声道最终化。
8. `M61-RT-011`：完整 RF/混合负载 soak 验收。

每关闭一项，提交信息或 PR 描述应引用风险编号，并附对应自动测试或硬件记录。
