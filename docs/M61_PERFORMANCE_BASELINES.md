# M61 性能基准与最优固件账本

更新日期：2026-07-15

本文件只记录真机可复现结果和晋升规则。优化路线、芯片事实和风险分析仍以
`M61_FULL_PERFORMANCE_OPTIMIZATION_SPEC.md` 为准。

## 1. 固定测试套件

### 1.1 `full-duplex-v1`（主排名）

固定运行 90 秒，同时保持：

- Windows USB speaker：48 kHz、4 通道端点，speaker 振幅 600；
- haptics 振幅 4000；
- HID 输出间隔 20 ms；
- Windows WASAPI DualSense mic 输入端点持续打开并排空；
- 固件请求真实 BT mic、解码真实 71 B Opus，并输出 USB IN；
- 不启用 synthetic decoder benchmark。

命令：

```powershell
python tools/run_m61_full_load.py --serial-port COM5 --mic-input `
  --status-log artifacts/full-duplex-v1.log
```

这是后续固件容量比较的主基准。mic、speaker、haptics、HID 必须同时工作；不能用静音跳过、
停发、降低质量或暂停另一方向来获得成绩。

### 1.2 `codec-isolation-v1`（算法辅助排名）

使用相同 speaker、haptics 和 HID 负载，但不开 Windows mic，改为每个 speaker epoch 解码
固定的 71 B fixture：

```powershell
python tools/run_m61_full_load.py --serial-port COM5 --decoder-bench `
  --status-log artifacts/codec-isolation-v1.log
```

它只用于比较 Opus/CELT、cache 和算术候选。它不覆盖 BT mic ingress、USB IN、真实内容复杂度
和两个时钟域的抖动，不能代替主排名。

## 2. 晋升与文件位置

固定位置：

- `artifacts/m61-best/full-duplex-v1/current/`
- `artifacts/m61-best/codec-isolation-v1/current/`

只有实际刷入并完成对应 90 秒测试的同一份 `.bin/.elf/.map` 才能进入 `current`。每次晋升
必须一并保存 before/after 状态日志、固件 SHA256、Git commit、构建 profile 和主观质量结论。
禁止用“相同源码后来重编译”的二进制替代已测试二进制。

`full-duplex-v1` 的硬门槛：

- speaker/haptics epoch drop、deadline、cancel、encode error 全为 0；
- BT send error、stale、drop、reject 全为 0；
- mic decode error、Opus/PCM drop、USB IN underflow 全为 0；
- Windows 输入回调错误为 0，采样非零；
- speaker、mic 和 haptics 主观测试均无卡顿、炸音或可辨失真；
- 不降低任何质量配置；
- 在上述条件都满足后，才按总 cycles、P99、max 和 cache miss 决定是否替换最优项。

如果候选更快但违反任一硬门槛，只记录在历史表，不替换 `current`。

## 3. 当前最优项

| 套件 | 状态 | 提交/构建 | 核心结果 | 固件位置 |
| --- | --- | --- | --- | --- |
| `full-duplex-v1` | 暂无合格最优项 | — | 首轮真实全双工存在明确丢包和欠载 | 待首次合格候选 |
| `codec-isolation-v1` | 当前算法最优 | `72fe7a1`, Opus 1.2.1 fixed, O2+LTO, `pvq-mdct-clusters` | encode 1,357,295 cycles；decode 845,589 cycles；合计 2,202,884 cycles/10 ms（68.84%）；全部实时错误为 0 | 历史测试未保留完整二进制，下一次合格复测后填充 `current` |

`codec-isolation-v1` 的证据日志为
`artifacts/pow2-div-core-hpm-90s_before.log` 和
`artifacts/pow2-div-core-hpm-90s.log`。该历史固件只保留了 SHA256 前缀
`AE66E5A1…`，因此不伪造完整 manifest。

## 4. 真实全双工首轮基线（失败，不晋升）

测试固件：提交 `441e0d8` 工作树加 mic diagnostic profile；SHA256
`5E6D0FF5D3836EDE805E25E4D0F049991334A4C1E7715FCAD2F8FBCEFA6AD4DE`。

证据：`artifacts/full-duplex-mic-baseline-20260714_before.log` 和
`artifacts/full-duplex-mic-baseline-20260714.log`。

本轮开始前已有人工听感测试，因此 percentile 和未刷新的累计 max 不能作为干净排名；区间
平均值和前后计数仍然有效：

| 指标 | 90 秒区间结果 |
| --- | ---: |
| speaker encode | 3,772.292 us / 1,207,799.959 cycles |
| 真实 mic decode | 3,237.701 us / 1,034,825.704 cycles |
| codec 合计 | 2,242,625.663 cycles/10 ms，约 70.08% 单核 |
| speaker/haptics epoch drop | 142 / 8,438，约 1.683% |
| BT mic Opus / decoded / decode error | 9,339 / 9,339 / 0 |
| mic PCM 1 ms 包 | 91,807 / 93,390，短缺 1,583 包 |
| USB IN underflow | 1,585 / 93,389，约 1.697% |
| Windows capture | 4,321,440 帧，status event 0，非零采样 |

当前 `audio_mic_opus_dropped` 同时混入 mic Opus queue 和 PCM packet-ring 溢出，区间增加
1,087 次，不能解释为 1,087 个 Opus 包均被丢弃。由于 decoded 数等于收到的 Opus 数，且
PCM 恰好短缺约 1,583 个 1 ms 包，本轮主要 mic 破音证据指向解码后 10 包突发写入 16 包
USB ring 时的溢出，随后又产生对应 underflow。Opus decoder 返回错误为 0，当前证据不支持
先把炸音归因于 Opus 编解码质量。

## 5. Mic FIFO 快速路径与 32 ms 抖动余量（有效，不晋升主最优）

测试固件 SHA256：
`D3BC074221A2FAA084A63052A29B3A69A0D13FF6552BAD088FB2A37F94550CD8`。

证据：

- `artifacts/full-duplex-mic-fifo32-aligned32-20260714_before.log`
- `artifacts/full-duplex-mic-fifo32-aligned32-20260714.log`

改动包括：

- mic USB PCM ring 从 16 个 1 ms 包扩展到 32 个，给 10 包突发生产与 1 ms DMA 消费提供
  22 ms 相位/调度余量；
- producer/consumer 改为严格 FIFO 游标，删除每包最多 32 槽的关中断扫描；
- mono→stereo 扩展改为单次 32 位写；
- producer 保存每槽 nonzero 元数据，删除 USB IN 热路径的 192 B 重扫；
- `audio_codec_task` 固定到 32 B 地址边界，避免同一任务因前置函数尺寸变化而任意漂移。

| 指标 | 首轮失败基线 | 本候选 |
| --- | ---: | ---: |
| mic PCM 1 ms 包 | 91,807 / 93,390 | 93,360 / 93,360 |
| mic PCM shortfall | 1,583 | 0 |
| mic ring/Opus drop 复合计数 | 1,087 | 0 |
| USB IN underflow | 1,585 / 93,389（1.697%） | 7 / 93,367（0.007%） |
| mic decode error | 0 | 0 |
| Windows capture status event | 0 | 0 |
| speaker/haptics epoch drop | 142 | 242 |

该候选解决了解码后 PCM ring 的持续溢出/欠载对，但 speaker/haptics epoch drop 仍非零，
因此属于有效的 mic 流水线优化，不替换 `full-duplex-v1/current`。剩余 7 个 underflow 很可能
来自 USB IN 打开后、首个 10 ms Opus 帧完成解码前的启动窗口，后续需用区分 startup/runtime
的计数器核实，不能直接记为运行期零欠载。

## 6. Speaker epoch 8 槽抖动余量（有效，不晋升主最优）

优化提交：`perf(m61): add speaker epoch jitter headroom`。测试固件 SHA256：
`28240EE88FDC721F615051ACDE1C5470901CA6F4E618631FB09DDAE06F8EB881`。

证据与本次实际测试二进制：

- `artifacts/m61-history/full-duplex-speaker-epoch8-20260715/`

speaker epoch ring 从 4 槽扩展到 8 槽。4 槽在正常流水线中会同时被 USB 填充、Opus
编码以及等待相邻配对发送的两个 epoch 占满，没有余量吸收一次 codec 或 BT 调度抖动；
8 槽增加约 9.4 KiB OCRAM，但不改变正常发送节奏、Opus 配置、音质或端到端路径。

| 指标 | 4 槽 FIFO32 基线 | 8 槽候选 |
| --- | ---: | ---: |
| speaker/haptics epoch drop | 242 | 0 |
| deadline / cancel / encode error | 0 / 0 / 0 | 0 / 0 / 0 |
| BT send error / stale / no-connection | 0 / 0 / 0 | 0 / 0 / 0 |
| codec cycles / 10 ms | 2,365,319（73.916%） | 2,366,861（73.964%） |
| mic Opus / PCM drop | 0 / 0 | 0 / 0 |
| mic PCM shortfall | 0 | 0 |
| USB IN underflow | 7 / 93,367（0.007%） | 9 / 93,370（0.010%） |
| Windows capture status event | 0 | 0 |

本候选消除了剩余 speaker/haptics epoch drop，且 codec 成本基本持平（+0.066%）。但 USB
IN 仍在启动窗口出现 9 个 underflow，因此尚未满足 `full-duplex-v1` 的全零硬门槛，不替换
`current`。下一步应把 USB Audio IN 启动期与稳定运行期 underflow 分开计数，并在首个真实
mic PCM frame 到达后再启动连续 VDMA 供给，不能用静音或停包掩盖运行期欠载。

## 7. Opus forward FFT E907 复乘快路（有效，不晋升主最优）

优化提交：`perf(opus): accelerate E907 forward FFT complex multiply`。测试固件 SHA256：
`945B1CB428A3EA766DAC06C09508C0E89A4CDF01BDA86129C77B07C9D195F794`。

证据与本次实际测试二进制：

- `artifacts/m61-history/full-duplex-opus-fft-forward-only-20260715/`

候选利用 E907 packed-DSP 的 `kmmwt2`、`kmmawb2` 和 `kmmawt2`，把 CELT forward FFT
复乘中的独立 Q15 乘法与后续加减融合。inverse FFT 的 `C_MULC` 保持原实现；此前同时修改
forward/inverse 的版本会使 decode 回退并产生 BT stale，已否决且未提交。码率、带宽、
complexity、帧长和固定点缩放均未改变。

| 指标 | 8 槽基线 | 候选首轮 | 候选复测 |
| --- | ---: | ---: | ---: |
| encode 平均延迟 | 3,999 us | 3,946 us | 3,962 us |
| encode P50/P95/P99 | 4,250/5,250/5,500 us | 4,250/5,250/5,250 us | 4,250/5,250/5,250 us |
| encode 最大延迟 | 5,748 us | 5,700 us | 5,806 us |
| encode cycles | 1,280,806 | 1,263,341（-1.36%） | 1,268,597（-0.95%） |
| decode 平均延迟 | 3,393.8 us | 3,306.8 us | 3,400.2 us |
| decode P50/P95/P99 | 3,750/4,750/5,000 us | 3,750/4,500/4,750 us | 3,500/4,500/4,750 us |
| decode 最大延迟 | 5,425 us | 5,644 us | 5,644 us（开机累计上界） |
| decode cycles | 1,086,055 | 1,058,275（-2.56%） | 1,088,231（+0.20%） |
| codec cycles / 10 ms | 2,366,861（73.964%） | 2,321,616（72.550%） | 2,356,828（73.651%） |
| epoch / Opus / PCM drop | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| BT stale / send error | 0 / 0 | 0 / 0 | 0 / 0 |
| USB IN underflow | 9 | 9 | 0 |

两轮 encode cycles 和 P99 均稳定改善，全链路无 drop、stale 或 codec error；复测总 codec
cycles 仍改善 0.42%。因此作为 encoder/forward FFT 有效优化保留。由于主排名仍要求全部
测试轮次 USB IN underflow 为 0，本候选暂不替换 `full-duplex-v1/current`。
## 8. 2026-07-15 PVQ shape audit and clean baseline

当前正式基线镜像 SHA256：`A5B3F9ADCFC334EF01D51EDCA713872B0566476AEDF661FB635E407E642E9C44`。
证据：`artifacts/m61-history/full-duplex-opus-post-pvq-baseline-20260715/`。

本轮为 90 秒真实全双工，保持 48 kHz、160 kbps、speaker mono 和 mic stereo，不改变
质量或包型。Encode 平均 `3.977 ms`、P50/P95/P99 `4.25/5.25/5.75 ms`、最大
`6.061 ms`，平均 `1,274,487 cycles`；Decode 平均 `3.424 ms`、P50/P95/P99
`3.50/4.75/5.00 ms`、最大 `5.488 ms`，平均 `1,096,884 cycles`。

真实 mic Opus `11,014` 包，decode error/drop 均为 0；BT 实时发送 `4216/4218`、
stale 增量 `2`，USB IN underflow 增量 `10`。该轮用于当前基线记录，不晋升或替换历史
最优提交。

PVQ 诊断统计显示 decode 热组合为 `(N,K)=(4,9)`、`(4,31)`、`(4,2)` 和 `(16,12)`。
`N=4,K=9` 专用入口实验因改变 ITCM 与 Opus state 地址而否决，未刷写；诊断能力提交为
`34e0af9`。

## 9. Codec 2-tick bridge window（稳定性最优）

测试固件 SHA256：
`F2C3105FA8098314781BFD2BDDAB7B98EF222224686F210CD87040BB5AF1EB2F`。

证据：

- `artifacts/full-duplex-codec-delay2-r1-20260715_before.log`
- `artifacts/full-duplex-codec-delay2-r1-20260715.log`
- `artifacts/full-duplex-codec-delay2-r2-20260715_before.log`
- `artifacts/full-duplex-codec-delay2-r2-20260715.log`

本候选保持 codec task 和 BT bridge 的原优先级层次，将 codec 每轮结束后的主动阻塞从
1 tick 增至 2 ticks；同时 `usb_unlock()` 在读取 IRQ mask 结束 cycle 后先恢复中断，再更新
HPM 最大值，避免性能统计自身延长关中断窗口。Opus 参数、码流、PCM、USB 包和音质均未改变。

| 指标 | 第 1 轮 | 第 2 轮区间 |
| --- | ---: | ---: |
| encode 平均延迟 | 3,825 us | 3,866 us |
| encode P50/P95/P99 | 4,250/5,250/5,500 us | 4,250/5,250/5,750 us |
| encode 最大延迟 | 5,839 us | 5,862 us |
| encode cycles / instret | 1,227,549 / 207,402 | 1,240,555 / 208,453 |
| decode 平均延迟 | 约 3,618 us | 3,599 us |
| decode P50/P95/P99 | 4,000/4,750/5,000 us | 3,750/4,750/5,000 us |
| decode 最大延迟 | 5,169 us | 5,609 us |
| decode cycles / instret | 约 1,153,000 / 195,800 | 1,152,245 / 196,175 |
| speaker/haptics qdrop/deadline | 0 / 0 | 0 / 0 |
| Opus/PCM drop 或 codec error | 0 | 0 |
| BT stale/retry/drop/reject/error | 0 | 0 |
| USB IN underflow 增量 | 10 | 1 |

连续两轮 90 秒真实全双工的运行期硬错误均为 0。USB IN underflow 只出现在 Windows
重新配置 Audio IN 接口后的首帧预热窗口；两轮结束时 mic ring 分别保持 4,032 B 和
4,224 B，且运行期间没有 mic Opus drop、PCM drop 或 decode error。因此本候选记录为当前
稳定性最优固件，但在启动 underflow 尚未独立计数前，不替换要求原始 underflow 计数全零的
`full-duplex-v1/current` 主性能排名。

## 10. 单 codec 阶段自适应 BT 恢复窗口（当前调度最优）

优化提交：`72ea56b`。证据：

- `artifacts/full-duplex-adaptive-window-r1-20260715.log`
- `artifacts/full-duplex-adaptive-window-r2-20260715.log`

保持 encode+decode 同轮后的 2 ms 确定性 BT 窗口；只有一轮恰好执行一个重型 codec
阶段时缩短为 1 ms，空闲轮仍为 2 ms。该策略不增加轮询频率，不改变 Opus 参数、码流、
音质、USB负载或任务优先级，只缩短队列积压后的恢复时间。

| 指标 | 固定 2 ms 候选 | 自适应窗口第1轮 | 自适应窗口第2轮区间 |
| --- | ---: | ---: | ---: |
| encode 平均延迟 | 4,057 us | 3,960 us | 约 4,030 us |
| encode P50/P95/P99 | 4,250/5,500/5,750 us | 4,250/5,250/5,500 us | 4,250/5,250/5,500 us |
| encode 最大延迟 | 6,030 us | 5,875 us | 6,068 us |
| decode 平均延迟 | 3,541 us | 3,513 us | 约 3,555 us |
| decode P50/P95/P99 | 4,000/4,750/5,000 us | 4,000/4,750/5,000 us | 4,000/4,750/5,000 us |
| decode 最大延迟 | 6,101 us | 5,240 us | 5,396 us |
| mic Opus queue 最大年龄 | 16,824 us | 13,121 us | 未刷新（仍为 13,121 us） |
| speaker/haptics/Opus drop | 0 | 0 | 0 |
| BT stale/retry/drop/reject | 0 | 0 | 0 |
| USB IN underflow 增量 | 32（启动累计） | 30（启动累计） | 0 |

mic queue最大年龄下降约22%，encode P95/P99与decode max同步改善，连续两轮没有运行期
硬错误。该提交因此晋升为当前调度最优；稳定回退点 `d314597` 继续保留。

## 11. 512→480精确相位累加重采样（当前流水线计算最优）

优化提交：`1c1b6bf`。证据：

- `artifacts/full-duplex-resample-phase-r1-20260716.log`
- `artifacts/full-duplex-resample-phase-r2-20260716.log`
- `artifacts/full-duplex-resample-phase-r3-20260716.log`

将每个输出样本重复计算的 `out*512/480` 商和余数替换为精确相位递推。480个相位已由
`tools/test_m61_resample_phase.py`穷举证明与原公式完全一致；线性插值、左右声道混合、
负数舍入、PCM结果、Opus参数和协议均未改变。

| 指标 | 第1轮 | 第2轮区间 | 第3轮区间/累计尾部 |
| --- | ---: | ---: | ---: |
| encode平均延迟 | 4,008 us | 约3,936 us | 约3,994 us |
| encode平均cycles | 1,283,470 | 约1,259,164 | 约1,278,416 |
| encode P50/P95/P99 | 4,250/5,250/5,750 us | 同左 | 同左 |
| encode max | 5,867 us | 5,946 us | 未刷新 |
| decode平均延迟 | 3,492 us | 约3,453 us | 约3,406 us |
| decode平均cycles | 1,115,871 | 约1,103,267 | 约1,087,085 |
| decode P50/P95/P99 | 4,000/4,750/5,000 us | 3,750/4,750/5,000 us | 累计3,750/4,500/5,000 us |
| mic queue最大年龄 | 15,296 us | 未刷新 | 未刷新 |
| 全部codec/queue/BT硬错误 | 0 | 0 | 0 |
| USB IN underflow累计/区间 | 34（启动） | +0 | +0 |

连续三轮满载稳定，且第二、三轮没有Audio IN运行期underflow。该提交记录为当前流水线
计算最优；最低mic queue峰值仍属于调度提交`72ea56b`的13,121 us，两项分别保留。

## 12. Codec HPM与USB锁诊断解耦（当前综合最优）

优化提交：`b3ee19d`。证据：

- `artifacts/full-duplex-no-lock-hpm-r1-20260716.log`
- `artifacts/full-duplex-no-lock-hpm-r2-20260716.log`

正式codec HPM固件继续统计encode/decode的wall time、cycles、instret和I/D-cache；每次
`usb_lock()`的两次`mcycle`读取及最大值维护只在重型pipeline profile中启用。音频、锁、
IRQ恢复顺序、Opus、调度和协议均未改变。正式测试中的`irq_mask_cycles_max=0`表示该诊断
未编译，不表示中断一直未屏蔽。

| 指标 | 第1轮 | 第2轮区间/累计尾部 |
| --- | ---: | ---: |
| encode平均延迟 | 3,967 us | 约3,818 us |
| encode平均cycles | 1,269,467 | 约1,222,505 |
| encode P50/P95/P99 | 4,250/5,250/5,750 us | 累计4,250/5,250/5,500 us |
| encode max | 5,842 us | 未刷新 |
| decode平均延迟 | 3,408 us | 约3,428 us |
| decode平均cycles | 1,088,390 | 约1,095,445 |
| decode P50/P95/P99 | 3,750/4,500/4,750 us | 同左 |
| decode max | 5,516 us | 未刷新 |
| mic queue最大年龄 | 10,525 us | 11,741 us |
| codec/queue/BT硬错误 | 0 | 0 |
| USB IN underflow累计/区间 | 46（启动） | +1（端点重开） |

该提交同时降低codec成本、P99和mic排队峰值，连续两轮无运行期硬错误，因此取代
`72ea56b`和`1c1b6bf`成为当前综合最优；两者继续作为调度与重采样的独立历史节点保留。

## 13. Codec单写者诊断无IRQ屏蔽更新（当前codec计算最优）

优化提交：`0136396`。证据：

- `artifacts/full-duplex-lockless-codec-diag-r1-20260716.log`
- `artifacts/full-duplex-lockless-codec-diag-r2-20260716.log`

`usb_diag`为volatile逐字段快照；encode耗时/计数、mic queue年龄和decode计数仅由codec
任务写入，不保护队列或音频数据。移除这些字段更新周围的IRQ save/restore；epoch队列、
mic Opus队列和USB PCM ring的所有权锁全部保留。

| 指标 | 第1轮 | 第2轮区间/累计尾部 |
| --- | ---: | ---: |
| encode平均延迟 | 3,877 us | 约3,809 us |
| encode平均cycles | 1,240,571 | 约1,219,739 |
| encode P50/P95/P99 | 4,250/5,250/5,750 us | 累计4,250/5,250/5,500 us |
| encode max | 5,787 us | 未刷新 |
| decode平均延迟 | 3,411 us | 约3,467 us |
| decode平均cycles | 1,090,062 | 约1,107,105 |
| decode P50/P95/P99 | 3,750/4,500/4,750 us | 同左 |
| decode max | 5,080 us | 累计5,197 us |
| mic queue最大年龄 | 12,792 us | 未刷新 |
| codec/queue/BT硬错误 | 0 | 0 |
| USB IN underflow累计/区间 | 29（启动） | +0 |

相对`b3ee19d`，encode成本和decode最大值继续下降；mic queue峰值轻微回升但仍处于稳定
区间，连续两轮无硬错误。因此该提交晋升为当前codec计算最优。

## 14. Mic PCM逐包链式预留（当前mic流水线最优）

优化提交：`5f7ea20`。证据：

- `artifacts/full-duplex-mic-pcm-chain-r1-20260716.log`
- `artifacts/full-duplex-mic-pcm-chain-r2-20260716.log`

每个10 ms decode产生10个1 ms USB PCM包。原实现每包分别锁一次预留、锁一次发布，共20次；
新实现首次预留后，在发布当前包的同一临界区预留下一个包，总计11次。每个包仍在填充后
立即转为READY，没有等待整批完成；USB ISR、FIFO顺序、ring深度和PCM数据均不变。

| 指标 | 第1轮 | 第2轮区间/累计尾部 |
| --- | ---: | ---: |
| encode平均延迟/cycles | 3,928 us / 1,257,004 | 约3,871 us / 1,239,846 |
| encode P95/P99/max | 5,250/5,500/6,110 us | 累计同左 |
| decode平均延迟/cycles | 3,331 us / 1,063,306 | 约3,453 us / 1,101,896 |
| decode P95/P99/max | 4,500/4,750/5,303 us | 累计4,500/4,750/5,439 us |
| mic queue最大年龄 | 11,283 us | 11,289 us |
| mic/codec/BT硬错误 | 0 | 0 |
| USB IN underflow累计/区间 | 30（启动） | +0 |

codec计时不包含PCM发布，因此总codec cycles与`0136396`基本持平；实际收益是每个decode
少9次共享临界区，并表现为更低的decode平均成本与mic queue峰值。该提交记录为当前mic
流水线最优；`0136396`继续保留codec最大延迟最优标签。

## 15. USB IN完成与下一DMA单锁续传（有效USB路径优化）

优化提交：`951253a`。证据：

- `artifacts/full-duplex-usb-in-single-lock-r1-20260716.log`
- `artifacts/full-duplex-usb-in-single-lock-r2-20260716.log`
- `artifacts/full-duplex-usb-in-single-lock-r3-20260716.log`

USB IN每1 ms完成回调原先先锁一次释放旧PCM槽，随后`arm_audio_in()`再次加锁选择下一槽。
新实现复用完成回调已有临界区准备下一DMA，锁外调用USB驱动；启动失败仍按原状态机回滚。
FIFO、underflow、静音启动buffer及DMA槽所有权不变。

三轮均为mic/codec/BT硬错误0，underflow只在首轮启动累计32，第二、三轮增量均0；Audio IN
持续输出，ring最终分别为3,840/4,416/4,032 B。累计encode P99/max为5,750/5,845 us，
decode P95/P99/max为4,500/5,000/5,508 us，mic queue max 13,949 us。该提交保留为USB
路径优化，但codec尾延迟最优仍为`0136396`，mic queue最优仍由更低峰值候选单独记录。
