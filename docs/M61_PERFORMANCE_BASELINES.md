# M61 性能基准与最优固件账本

更新日期：2026-07-14

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
