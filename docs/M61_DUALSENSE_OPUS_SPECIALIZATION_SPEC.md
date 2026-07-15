# M61 DualSense 专用 Opus/CELT 无损质量优化规格

状态：设计冻结，待分阶段实施与真机 A/B

版本：1.0

日期：2026-07-15

目标平台：Ai-M61-32S-Kit / BL616，原装 DualSense 蓝牙链路

## 1. 目标

在不降低音频质量、不改变 DualSense 协议语义、不隐藏丢包的前提下，降低 speaker Opus
encode、mic Opus decode 和共享 CELT 内核的平均成本、P99 与最大延迟，最终消除扬声器、
触觉和麦克风并发时的颗粒化、爆音和“盖革计数器”式断续。

当前生产路径已固定为 48 kHz、10 ms、单声道、160 kbps CBR、mediumband、
`OPUS_APPLICATION_RESTRICTED_LOWDELAY`、complexity 0。公开 Opus 低成本旋钮已经用尽，后续
收益必须来自位精确底层优化、固定包型专用化和无用通用分支裁剪，不能继续降低配置。

## 2. 不可违反的质量与协议约束

- 禁止降低采样率、有效频带、码率、声道语义、帧长精度或 PCM 位深。
- 禁止通过静音检测跳过编码、暂停 mic、丢弃 speaker、减少 haptics 包或放宽 deadline
  获得性能数字。
- 禁止把 Opus complexity、bitrate、bandwidth、VBR/CBR、FEC、PLC 或 packet loss 配置
  的变化伪装为算法优化。
- speaker 输出仍必须是手柄接受的合法 Opus 位流；mic 对所有真实捕获包必须保持兼容。
- 位精确候选必须对参考 Opus 1.2.1 产生完全相同的 packet bytes 或 PCM samples。
- 非位精确候选默认禁止。只有数学重排不可避免时，必须证明所有定点边界、饱和、舍入、
  溢出语义相同；无法证明则淘汰，不以主观“听不出”放行。
- Bluetooth realtime accepted/transmitted 必须完全相等；stale、retry-drop、send error、
  epoch drop、Opus drop、PCM drop、codec error 和运行期 underflow 必须为 0。
- 灯带、trigger、speaker、haptics、mic 和普通 HID 输入均必须真机可见/可听/可采集，不能只
  依据本机发送计数判断协议通过。

## 3. 固定正式基准

正式性能 A/B 始终使用 `tools/run_m61_full_load.py` 的冻结默认负载：

- duration 90 s；48 kHz；block 480 frames；
- speaker amplitude 600；haptics amplitude 4000；
- HID interval 20 ms；mic input enabled；
- 同一手柄、同一连接方式、同一 CPU/Flash 时钟和相同构建 profile。

主观验收另设高可感知幅度与连续低频/扫频用例，但不得混入正式性能排名。每个候选至少两轮
90 s；任一轮失败即不晋升。明显协议或实时错误候选不复测。

## 4. Phase 0：测量可信度与包型审计

在继续优化 Opus 前必须完成：

1. 状态快照同时输出 encode/decode 的 `samples`、`total_us`、`total_cycles`、
   `total_instret`、last、max 和 histogram 总样本数。
2. 增加不变量检查：`histogram_sum == samples`；P50 不得长期明显高于平均值两倍；
   `total / samples` 必须等于打印平均值。
3. 若不变量失败，记录独立 diagnostic counter，禁止该轮进入排名。
   当前状态三元组的既有顺序为 `last/average/max`；所有输出必须在字段名中显式标注，禁止
   再依赖隐含顺序解释。
4. 审计 HPM 是否统计了 codec 被中断/抢占期间的其他任务成本；wall time 与 core cycles 分开
   解释，不把调度成本冒充 Opus 指令成本。
5. 记录真实 DualSense mic 包的 TOC、payload length、frame count、bandwidth、mode、channels
   和变化次数，只记录统计摘要，不打印原始麦克风内容。
6. 记录 speaker 输出 TOC/长度，确认每帧固定条件，而不是依据配置代码推断。

Phase 0 只增加测量，不改变 codec、包内容和调度行为，完成后单独提交。

## 5. Phase 1：建立 DualSense 固定配置专用入口

基于 Phase 0 证明过的固定条件，为 speaker 建立显式专用入口：

- 48 kHz、480 samples、mono、restricted-lowdelay、CBR、160 kbps、mediumband；
- 初始化阶段预计算并冻结不会改变的 mode、frame size、bandwidth、channel 与 allocation 参数；
- 删除每帧重复执行、且已由断言证明恒定的 API 参数验证和模式选择；
- 保留标准 `opus_encode()` 作为编译期开关和 A/B 参考；
- 专用入口输出 packet bytes 必须逐帧等于标准入口。

不得直接复制私有结构偏移。专用 API 必须位于同一固定版本 Opus 源码内，并用静态断言约束
结构、mode 和 frame size。升级 Opus 版本时默认关闭，重新审计后才能启用。

## 6. Phase 2：DualSense mic 固定包型 decoder 快路

只有当 Phase 0 证明真实 mic 包型长期固定后实施：

当前 90 秒审计证据为 speaker `8438/8438` 全部 `TOC=0xB0`、200 B、mono，mic
`13960/13960` 全部 `TOC=0xD4`、71 B、stereo；两者 TOC 和长度变化均为 0。首个 decoder
候选只针对 `0xD4/71 B`，其余包全部回退 upstream parser。

- 固定 TOC、单帧、10 ms、单声道及真实观测 bandwidth/mode；
- 跳过已由 packet metadata 验证过的通用多帧、多声道、SILK/Hybrid 分派；
- 对任何不匹配包立即回退标准 `opus_decode()`，不得拒绝合法变化包；
- packet loss、PLC、FEC 和 malformed packet 的标准行为必须保留并有测试覆盖；
- fast path 与标准 decoder 的 24 万帧以上 PCM checksum 必须完全一致。

## 7. Phase 3：共享 CELT 位精确热点

按真机阶段 HPM 顺序优化，不凭函数名猜测：

1. inverse MDCT/FFT 循环与复乘依赖链；
2. PVQ decode 的 `decode_pulses`、`quant_partition`、`quant_band`；
3. range decoder 热循环；
4. deemphasis、denormalisation 和 output copy；
5. forward MDCT/FFT 与 PVQ encode 中仍可共享的定点算术。

每个 DSP 指令或算术重排候选必须通过：

- 全部关键边界值；
- 至少 100 万随机输入；
- 至少 1200 帧 encoder packet byte equality；
- 至少 24 万帧 decoder PCM equality；
- UBSan/host reference（可运行部分）；
- 两轮真机全负载。

只减少 instret 但 cycles、P99 或 max 不稳定下降的候选不得保留。

## 8. Phase 4：构建级裁剪与布局

在专用路径验证后，才允许裁剪当前固件永远不可达的 Opus 功能：

- 未使用的浮点实现、multistream API 和 encoder 应用模式；
- 经 TOC 审计证明 speaker 不使用的 SILK/Hybrid encoder 路径；
- 其他采样率、frame duration 和 channel 配置的 encoder-only 代码。

mic decoder 仍必须接受真实手柄可能发送的全部合法包型；没有长期捕获证据不得裁剪 decoder
模式。裁剪目标首先是减少 I-cache footprint 和链接体积，不把 ROM 体积下降直接视为性能收益。

任何 RAM/TCM 放置必须记录物理存储体、cache alias、函数地址、Opus state 地址和后续 BSS
地址。禁止再次把单函数搬迁和 cache-color 变化混成不可归因候选。

## 9. 质量验证矩阵

### 9.1 位流与 PCM

- impulse、silence、full-scale、alternating extremes；
- 20 Hz–20 kHz sweep；
- 单频、双频、白噪声、粉红噪声；
- 随机 PCM 与真实游戏/语音片段；
- encoder packet bytes equality；decoder PCM samples equality；
- packet loss、corrupt packet、unexpected TOC 和 mode fallback。

### 9.2 真机功能

- 手柄扬声器连续可听，无周期点击、颗粒化或音高变化；
- haptics 连续且与音频相位稳定，无脉冲式停顿；
- mic 连续采集、非零且无 status event；
- 灯带、player LED、mute light、trigger 和 rumble 状态真实变化；
- USB HID 输入、feature report 和 Bluetooth 重连保持正常。

### 9.3 性能晋升

- 两轮 codec 总 cycles 均下降，且单项 encode/decode 不允许以明显恶化换总和；
- P99 和 max 不恶化超过测量噪声，任一实时错误为 0；
- 主观连续播放至少 5 分钟无“盖革计数器”式断续；
- 保存实际测试 `.bin/.elf/.map`、SHA256、before/after log 和构建参数。

## 10. 提交与回退规则

- Phase 0 测量修复、packet audit、speaker fast path、mic fast path、每个 CELT 内核优化分别提交。
- 只有通过全部硬门槛的有效优化才提交并更新 `docs/M61_PERFORMANCE_BASELINES.md`。
- 每个提交必须可独立关闭或回退；不得把多个算法候选、内存布局和调度改动混在一起。
- 失败候选保留证据日志，但源码恢复到已验证最优；不为失败候选创建性能提交。
- 当前恢复基线固件 SHA256：
  `945B1CB428A3EA766DAC06C09508C0E89A4CDF01BDA86129C77B07C9D195F794`。

## 11. 实施顺序

1. 修复并扩展性能原始总量/一致性诊断。
2. 采集 speaker/mic TOC 与包长分布，证明固定包型。
3. 实现 speaker 固定配置专用入口，要求 packet byte equality。
4. 实现带标准 fallback 的 mic decoder fast path，要求 PCM equality。
5. 依据阶段 HPM 逐个优化 inverse MDCT/PVQ/range decoder。
6. 最后评估不可达代码裁剪和稳定布局，不再优先尝试 CRC 或单函数搬 RAM。
