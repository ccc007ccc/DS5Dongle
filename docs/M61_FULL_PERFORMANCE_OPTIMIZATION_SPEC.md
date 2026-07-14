# M61 DualSense 固件全性能优化规格

状态：基线冻结，进入分阶段实施

版本：1.0

日期：2026-07-14

目标平台：Ai-M61-32S-Kit / BL616、BL618 兼容 SDK 目标

项目基线提交：`37a0015`

SDK 基线提交：`d9306a4a`

## 1. 结论摘要

本项目当前的主要限制不是 CPU 未超频、没有使用某个神秘 ROM API，也不是所有代码都应
搬入 RAM，而是以下四项叠加：

1. 单核 E907 在 320 MHz 下同时承担 USB、Opus、Bluetooth Classic、HID 和调度；当前
   单声道 speaker encode 已占约 39.8% 的每 10 ms 周期预算，旧版真实算法 mic decode
   约占 41.1%，两者并发后还会发生 I-cache 和任务抢占放大。
2. 当前音频质量配置仍是临时降级状态：speaker encoder 为单声道、强制 mono、限制为
   mediumband。最终目标必须恢复真实双声道和协议参考频带，不能把当前值当作完成状态。
3. USB ingress、512→480 重采样、CRC、bridge 轮询和 Bluetooth buffer 分配仍有未量化
   的 CPU、cache 和抖动成本；现有 HPM 主要覆盖 codec，而不是完整端到端路径。
4. 当前仍保留“期限不足时取消 speaker、只发 haptics”的 fallback。它会掩盖系统容量
   不足，违反全功能同时运行的目标，必须删除。

芯片与 RAM 的最终结论如下：

- CPU 已运行在原厂文档上限 320 MHz，Flash 已运行在 80 MHz QIO；不把超频作为路线。
- SDK 链接器中的 `.itcm` 只是 cached OCRAM 输出段，不是独立物理 ITCM。
- cached OCRAM 在 I-cache miss refill 时显著快于 Flash XIP，但不会自动减少 miss；函数
  布局和 cache set 冲突可能使搬 RAM 后反而更慢。
- 经过真机 A/B 的 PVQ 与 MDCT/FFT 连续调用簇应保留。它们把正式单声道 encode 基线从
  `5024 us / 1,609,825 cycles` 降到 `3971 us / 1,273,238 cycles`，cycles 改善约
  20.9%。除此之外禁止继续凭函数大小或“RAM 理论更快”猜测放置。
- 外置 4 MiB pSRAM 是容量工具，不是片上低延迟 RAM。它可用于迁移冷数据、日志和大缓冲，
  但热 Opus code/state 默认不得放入 pSRAM。

按收益、风险和依赖关系，下一阶段优先级是：补齐端到端测量并删除 speaker fallback；
优化 ingress 的逐样本加载和除法；在正式 RAM 布局下重建 decoder 基线；核实 USB/BT/Opus
时钟域和 512→480 cadence；然后优化重采样、CRC 和 bridge 调度。QoS、USB selective
FIFO、USB HS 和 pSRAM 均放在后期测量型实验阶段。

## 2. 文档权威、审计范围与硬约束

### 2.1 权威顺序

硬件结论按以下优先级裁决：

1. 四份原厂 PDF 的文字、图片、框图和原理图；
2. 当前使用的 Bouffalo SDK 源码、链接脚本、最终 ELF/反汇编和启动寄存器；
3. 同一固件、同一负载、同一连接条件下的真机 HPM 和端到端指标；
4. 外部参考实现只用于协议和实现线索；
5. 项目历史文档只能帮助定位旧实验，不能单独作为芯片事实。

本规格已核对 PDF 中的图片页，包括数据手册的 CPU/cache/总线框图、参考手册的系统总线、
USB 和存储器章节、Ai-M61 开发板接口图以及 NodeMCU 原理图。不能用 PDF 文本抽取结果替代
图片核对。

### 2.2 代码审计范围

项目侧审计覆盖当前仓库中除生成目录和外部分析副本外的 64 个 C/C++/汇编、头文件、构建
脚本和测试工具，重点包括：

- `m61/dualsense_hidp_probe/main.c`：连接、HIDP、bridge、BT 发送、任务和诊断；
- `m61_audio_epoch.*`：USB audio epoch、所有权、haptics、speaker pair；
- `m61_usb_gamepad.*`：USB composite、VDMA ingress、codec、mic/speaker USB 流；
- `m61_bt_tx_scheduler.*`：实时音频、状态报告和重试调度；
- `dualsense_output.*`、`dualsense_parser.*`：报告布局、CRC 和协议解析；
- Opus 1.2.1 构建脚本、E907 patch、RAM 放置 patch 和 profile hooks；
- 所有 host 回归、固定负载、刷写、日志比较和 ELF/RAM 检查工具。

SDK 审计覆盖本固件真实依赖闭包中的 BL616 启动、cache/sysmap、时钟、链接器、USB device
VDMA、FreeRTOS、Bluetooth net_buf/L2CAP、CRC 工具、pSRAM 和 GLB 总线寄存器。未使用的
图形、Wi-Fi 应用示例等不作为本项目性能结论来源。

### 2.3 不可违反的约束

- 不降低采样率、码率、声道数、频带或主观音质换性能。
- speaker 每 10 ms 固定 200 B Opus、160 kbps CBR；不得恢复 64 kbps 后 padding 的方案。
- 最终 speaker 必须是真双声道，不得用 mono 编码后复制两个声道伪装通过。
- mic 当前生产配置保持关闭，不请求手柄 mic 流、不运行真实 mic decode；但压力测试必须
  保留 decoder benchmark，最终必须支持 speaker stereo、mic、haptics 和输入并发。
- 禁止 speaker 活跃后暂停 mic decode 250 ms，禁止任何同类互斥式退让。
- 不把“不操作就不发包”“静音不编码”等空闲省算力计入最终容量收益。
- 算术优化优先位精确。非位精确候选必须证明相对参考质量没有客观下降，并同时通过真机
  P99、drop、deadline 和主观听感门槛。
- M61 固件允许使用已验证的 Windows 原生或 WSL Xuantie GCC 构建；跨宿主不得混用 GCC
  LTO archive。新宿主生成的固件必须先通过静态 gate 和真机 HPM 对照，才能替换性能基线。
  刷写后开发板仍需要人工 reset 或 USB 重新上电。
- 每个有效优化独立提交 Git；被否决实验可以提交测试结论，但不得留在正式默认配置。

## 3. BL616/BL618 硬件性能模型

### 3.1 CPU 与 cache

- Ai-M61-32S 模组资料标明主芯片为 BL618；当前 Bouffalo 工程使用 `CHIP=bl616` 的兼容
  SDK 路径。不能因此套用 `BL618DG`/`BL616CL` 专用的 8 FIFO、双核或其他变体能力；任何
  变体能力都必须先用芯片 ID、原厂手册和寄存器实测确认。
- CPU：T-Head E907，RV32IMAFCP，单发射、顺序执行；整数五级、浮点七级流水。
- CPU/cache/TCM 公开最高频率：320 MHz；系统总线公开最高频率：80 MHz。
- I-cache：32 KiB、2-way；D-cache：16 KiB、2-way；SDK cache line 为 32 B。
- 当前编译目标包含
  `-march=rv32imafcp_zpn_zpsfoperand_xtheade -mabi=ilp32f -mtune=e907`，可使用 E907
  packed-DSP 指令，但指令数减少不代表 cycles 必然减少，必须真机验收。

每个 10 ms 帧周期理论上有 3,200,000 CPU cycles。最终所有任务的最坏稳定预算不得超过
2,560,000 cycles/10 ms，保留至少 20% 余量给中断、cache 抖动和协议突发。

### 3.2 存储器与总线路径

| 区域 | CPU cache 地址 | non-cache 地址 | 实际结论 |
| --- | --- | --- | --- |
| OCRAM 320 KiB | `0x62FC0000` | `0x22FC0000` | cache alias 经 cache/AXI→AHB；non-cache alias 直走 AHB |
| WRAM 160 KiB | `0x63010000` | `0x23010000` | 与 OCRAM 相同的 alias 模型；部分空间由无线/EM 占用 |
| Flash XIP | `0xA0000000` | 无项目使用的别名 | 外置 Flash，当前 80 MHz QIO，I-cache miss refill 成本高 |
| pSRAM 4 MiB | `0xA8000000` | 无公开项目别名 | 外置 Winbond x8，cacheable/bufferable，6-clock latency、64 B wrapped burst |
| ROM | `0x90000000` 区域 | strong-order/non-cache | 当前 codec 热路径没有实际跳转到 ROM API |

CPU 有 AXI 和 AHB 两侧：Flash、ROM、pSRAM 主要位于 AXI 侧；USB、无线、DMA 和片上 SRAM
主要位于 AHB 侧。把 codec code 搬入 OCRAM 会降低 Flash refill 成本，但也会增加 AXI→AHB
桥接和片上 SRAM/cache set 压力，因此必须按连续调用簇而不是按单函数直觉放置。

### 3.3 `.itcm` 的真实含义

`bl616_common.ld.in` 的 `ram` 起点是 `0x62FC0000`。`.tcm_code.*` 被收集进名为 `.itcm`
的输出段，但该段仍位于同一 cached OCRAM/WRAM `ram` 区域。BL616 当前工程没有一个可供
应用自由放置、与 cache 分离的物理零等待 ITCM。

因此后续文档和代码中统一使用“cached OCRAM code placement”，不得用“进入 ITCM 所以
必然更快”作为结论。

### 3.4 USB 与 DMA

- BL616 USB device 自带 VDMA；SDK `bflb_usb_v2.c` 已用 VDMA 直接在 FIFO 和内存之间传输。
- 原厂 USB 有 4×512 B 非零 endpoint FIFO，并支持多缓冲配置。
- 本 composite 设备正好使用四个非零 endpoint：audio OUT EP1、audio IN EP2、HID OUT
  EP3、HID IN EP4。
- SDK 的 `CONFIG_USB_PINGPONG_ENABLE`/`CONFIG_USB_TRIPLE_ENABLE` 是全局 FIFO 映射模式。
  直接启用会让每个 endpoint 消耗多个 FIFO，无法同时保留当前四个 endpoint。
- 通用 DMA 只有 AHB master；USB 已有专用 VDMA，再套通用 DMA 不会减少 USB FIFO 传输，
  只会增加搬运和总线竞争。

若要双缓冲 audio OUT，必须修改 USB driver 为 selective FIFO mapping，只为 EP1 分配额外
FIFO，并证明 EP2/EP3/EP4 枚举、传输和中断都不受影响。该项不是近期默认优化。

### 3.5 板级资源

- Ai-M61 开发板 BOOT 为 GPIO2，可作为配对键；RGB 为 GPIO12/14/15。
- LED 当前使用硬件 PWM 调光，不占用实时音频任务轮询预算。
- 模组带 4 MiB pSRAM 和 8 MiB Flash。pSRAM 当前未启用 `CONFIG_PSRAM`。

## 4. SDK 已用能力与未决能力

### 4.1 已启用并应保留

| 能力 | 当前状态 | 决策 |
| --- | --- | --- |
| I-cache / D-cache | 启动代码已启用 | 保留 |
| D-cache write-back/write-allocate | 已启用 | 保留 |
| branch prediction、return stack、L0 BTB | SDK 启动已启用 | 不重复配置 |
| `MXSTATUS.THEADISAEE`、`MXSTATUS.MM` | `SystemInit()` 已启用 | 保留 |
| CPU 320 MHz | `board.c` 使用 WIFIPLL 320M | 已达文档上限 |
| Flash 80 MHz QIO | board 初始化后校准到 80 MHz | 保留并记录校准失败 |
| USB WIFIPLL 时钟 | 已启用 | 保留 |
| D-cache preload | 当前为 1 | 真机有效，保留 |
| D-cache AMR | 当前为 1 | 与 preload 互补，保留 |
| Opus O2 + LTO | 源码库默认 | 保留 |
| E907 `clz32` | 位精确/真机有效 | 保留 |
| E907 Q16 `smmwb` | 位精确/真机有效 | 保留 |
| E907 Q15 `kmmwb2` | 合法输入域位精确/真机有效 | 保留 |
| exact reciprocal | 客观质量无下降、真机有效 | 保留 `noinline` 约束 |

过去的 `smmwb+slli` Q15 候选会丢失 product bit 15，且产生最低位差异，已否决。当前正式
采用的是 Q16 的单条 `smmwb` 和 Q15 的 `kmmwb2`，两者不是旧的负面候选。

### 4.2 可测但不能直接启用

1. GLB `BMX_CFG0` 支持 fixed/round-robin arbitration，`BMX_CFG1` 有 CPU、SDU、DMA、BLEM
   等一位 QoS 字段。SDK 当前没有设置这些位，头文件也没有说明 0/1 的优先语义。只能先读
   启动值，再做单比特 A/B；禁止猜测后直接写生产配置。
2. HPM 可测 I-cache、branch/mispredict、D-cache read/write miss、TLB miss、store、cycle
   和 instret。当前主要测 encode/decode 的 I-cache 与 D-cache read，覆盖不足。
3. pSRAM 初始化能力存在，但必须先做顺序读写、随机延迟、cache miss 和 BT/USB 并发总线
   干扰微基准，再决定迁移冷对象。
4. USB HS 目前未启用。只有在原理图、PHY 和实际链路稳定性验证后才能实验；它不会直接
   降低 Opus 算法 cycles，不能当作 codec 优化。

### 4.3 ROM 结论

`bl_cpu_sysmap_init()` 把 `0x63038000..0xA0000000` 设为 strong-order/non-cache，其中包含
ROM 区域。最终反汇编确认当前 `arch_memcpy` 是 `0x62FC...` 的本地循环，标准
`memcpy/memset` 位于 Flash，codec 热路径未跳转到 `0x90000000` ROM。ROM API 不是当前
瓶颈，不进入近期路线。

## 5. 当前端到端数据流与调度

### 5.1 speaker/haptics 输出流

```text
Windows USB Audio OUT, 48 kHz, 4×int16
  -> BL616 USB EP1 VDMA, non-cache 8-slot ring
  -> audio_ingress_task
  -> m61_audio_epoch_ingest_usb()
       ch0/ch1: speaker PCM, 512-frame epoch
       ch2/ch3: 每16帧抽取为64 B haptics block
  -> audio_codec_task
       512 stereo PCM -> 480 mono PCM
       Opus 48 kHz / 10 ms / 1 ch / 160 kbps CBR / complexity 0
  -> 相邻两个 epoch 组成 547 B BT realtime report 0x39
  -> CRC32 + L2CAP net_buf + BR/EDR HID interrupt channel
  -> DualSense speaker + HD haptics
```

USB descriptor 宣告 48 kHz，但 codec 每积累 512 个 USB frame 就按 512→480 处理；外部参考
实现也把该段显式当作 51200→48000。该时钟域/cadence 关系尚未用 USB SOF、每包 frame 数、
epoch fill 时间和 BT send 时间共同证明。它可能影响长期缓冲相位、音高或周期性音色变化，
必须列为 P0 测量项，不能只因参考实现也这样写就视为正确。

### 5.2 mic 输入流

协议和代码已经有：BT 71 B Opus packet → mono 48 kHz decode → USB IN 双声道复制 packet
ring。但 `CONFIG_M61_DS5_MIC_DEFAULT_ENABLED=0` 时入口、active 状态和 decode 均被硬门禁，
当前生产固件没有打开 mic。profile 中的 decoder benchmark 只执行固定 71 B 解码并丢弃
PCM，不请求真实 mic，也不改变 USB IN 功能状态。

### 5.3 任务优先级

`configMAX_PRIORITIES=31` 时当前主要优先级为：

| 任务 | 优先级 |
| --- | ---: |
| FreeRTOS timer | 30 |
| app start | 29 |
| USB audio ingress | 28 |
| BT HCI TX | 28 |
| codec | 27 |
| BT RX / controller RX | 27 |
| bridge / USB EP0 | 26 |
| LED | 22 |
| shell | 5 |

已知无效布局：把 bridge 提升到 28 曾导致 HCI ACL 错误、Steam 仅枚举但无输入、DSX 识别
失败；把 codec 降到 24 会使 wall-time 因抢占从约 9.6 ms 变成 14.5 ms。后续不得凭感觉
调优先级，必须同时记录任务 runtime、HPM cycles 和抢占 wall-time。

当前 codec 每轮末尾固定 `vTaskDelay(1 ms)`，bridge 每 1 ms 无条件唤醒并扫描 ingress、
scheduler、host/feature queue 和 BT send。事件驱动 bridge 有实际意义，因为它能减少满载
之外的无效唤醒和满载时的 cache 污染，并用精确事件交付发送窗口；但它不是 codec 本体
提速手段，也不能以“空闲时不发包”作为最终收益。实现时必须保留 polling A/B 开关。

## 6. 当前质量状态与功能债务

当前 speaker codec 配置为：

- 48 kHz，10 ms；
- `OPUS_APPLICATION_RESTRICTED_LOWDELAY`；
- 160 kbps CBR，固定 200 B；
- complexity 0；
- encoder channels=1，force mono；
- max bandwidth=mediumband；
- 512→480 线性插值前先把左右声道平均成 mono。

协议参考实现使用 `opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO)`、160 kbps CBR、
complexity 0 和 51200→48000 WDL resampler。当前项目的 mono 与 mediumband 是性能妥协，
不是允许长期保留的质量配置。

最终质量要求：

1. 恢复真实双声道 encoder，左右输入不能提前平均。
2. 不强制 mediumband，按协议和客观听音验证恢复 fullband/参考带宽。
3. 保持 160 kbps、200 B/10 ms、CBR，不以 padding 掩盖低码率。
4. 新重采样器必须对左右声道分别处理，并与参考实现比较频响、alias、THD+N、SNR 和听感。
5. 输入单声道、反相声道、单边脉冲和扫频时必须证明没有错误串音、抵消或相位漂移。

## 7. 正式性能基线

### 7.1 当前保留配置

- Opus 1.2.1 fixed；
- O2 + LTO；
- complexity 0、160 kbps CBR；
- E907 `clz32`、exact-rcp、Q16 `smmwb`、Q15 `kmmwb2`；
- D-cache preload=1、AMR=1；
- PVQ 五函数紧密调用簇和 MDCT/FFT 调用簇放 cached OCRAM；
- 正式固件 SHA256：
  `6781749C3A88E80CCF85DC5C2B6C3EB8ED91ECCC3E518B2A7BC5A7AC5274EAA9`。

两轮加权结果：

| 指标 | 当前正式基线 |
| --- | ---: |
| encode wall-time 平均 | 3971 us |
| cycles/encode | 1,273,238 |
| instret/encode | 211,141 |
| I-cache miss/encode | 2607 |
| 累计 P99 | 5000 us |
| queue/drop/deadline/stale/encode error 增量 | 0 |

相对 selective RAM 前的 D-cache 正式基线
`5024 us / 1,609,825 cycles / 224,643 instret / 3146 I-miss`，时间和 cycles 改善约
20.9%，I-cache miss 改善约 17.1%。当前 ELF 的 `.itcm` 约 27,176 B，片上静态 RAM 约
195,952 B / 415 KiB，约 46.11%；USB non-cache 区约 7,816 B。

当前 mono encode 的 1.273 M cycles 等于单核每 10 ms 理论周期的 39.8%。旧 synthetic mic
decode 为 `4111 us / 1,313,852 cycles`，约 41.1%。两者简单相加已达 80.8% cycles，旧
并发测试的纯 codec wall-time 为 9.708 ms，并出现 qdrop、deadline 和 stale。正式 RAM
布局可能改善 decoder，但真实 stereo encode 会增加负载，因此当前没有开启 stereo+mic
的可靠余量。

### 7.2 已验证保留项

| 优化 | 结果 | 质量性质 |
| --- | --- | --- |
| source Opus O2 vs SDK archive | 平均约 -16.2% | 同版本、同协议 |
| O2 + LTO vs O2 | 再约 -8.8% | 同协议 |
| prefilter disabled fast path | 小幅有效 | encoder stream 位精确 |
| E907 `clz32` | cycles 约 -9.7% | 位精确 |
| exact reciprocal | cycles 约 -0.8%，P99 改善 | 非位精确，但相对原 PCM SNR 差异 <0.001 dB |
| Q16 `smmwb` | cycles 约 -0.69% | 位精确 |
| Q15 `kmmwb2` | cycles 约 -0.89%，instret -9.86% | 合法输入域位精确 |
| D-cache preload+AMR | cycles 约 -5% | 不改数据 |
| PVQ 连续簇 13,448 B 到 OCRAM | cycles 约 -8.5% | 只改放置 |
| MDCT/FFT 连续簇 3,082 B 到 OCRAM | 再约 -13.6% | 只改放置 |

### 7.3 已否决且不得直接重试

| 候选 | 真机结果/原因 |
| --- | --- |
| 64 kbps 后 padding 到 200 B | 出现周期性水下/桶盖音色，属于质量换性能 |
| 全库 O3 | I-cache 工作集扩大、P99 和 drop 恶化 |
| 整体 Opus 搬 `.itcm` | 函数布局/cache 冲突使性能下降 |
| `quant_all_bands` 单函数搬 OCRAM | cycles +0.66%，I-miss +2.16% |
| band energy 1,044 B 搬 OCRAM | cycles +7.21%，I-miss +12.86% |
| `tf_analysis` 1,464 B 搬 OCRAM | cycles +7.55%，I-miss +8.33% |
| Q15 `smmwb+slli` | 非位精确且 cycles 无收益 |
| `khmbb` 通用替换 | cycles 约 +5.37% |
| `mul+mulh` 精确 Q15 | cycles 约 +15%，I-miss 增加 |
| SMALDA 通用内积 | code 膨胀、cycles/P99/drop 恶化 |
| FFT/MDCT 固定表强制 32 B 对齐 | cycles 约 +4.8% |
| mono deemphasis 专用路径 | 真机仅约 0.3%，低于门槛且增加代码 |
| bridge 提升到优先级 28 | HCI/输入功能回归 |

Opus 1.6.1 和 float 变体不作为当前主线。项目冻结在已验证更适合 E907 的 Opus 1.2.1 fixed，
后续优先优化共享 CELT 底层；只有出现明确的上游修复或可量化热点收益时才重新建立新版 A/B。

### 7.4 全链路 profile 开销 A/B

`50f5325` 把 ingress work、resample、BT alloc/build/send、pair age、report interval、USB packet
cadence 和 epoch interval 全部绑入 core HPM 构建。相同 `baseline-v1` 90 秒负载下，encode 从
`3846 us / 1,233,540 cycles` 退化到 WSL 的 `4786 us / 1,530,782 cycles`；Windows 同源码
结果为 `4729 us / 1,512,719 cycles`，因此退化不是构建宿主导致。

将新增全链路统计拆到独立 `CONFIG_M61_PIPELINE_PROFILE`，仅保留原 core encode/decode/ingress
HPM 后，同一轮 90 秒结果恢复到：

| 指标 | 拆分前 | pipeline 关闭 | 改善 |
| --- | ---: | ---: | ---: |
| encode 平均 | 4786 us | 4047 us | 15.4% |
| cycles/encode | 1,530,782 | 1,296,022 | 15.3% |
| instret/encode | 223,726 | 212,061 | 5.2% |
| I-cache miss/encode | 3,326 | 2,631 | 20.9% |

该轮 `qdrop/odrop/cancel/deadline/stale/encode error` 全为 0，确认统计代码是退化主体。相对
`0eb2308` 单轮结果仍慢约 5.2%，剩余差异必须继续二分，不能全部归因于 profile。

### 7.5 ingress 位精确除法消除

反汇编确认 `-Os` 会在每个 haptics 抽样点保留 gain `/256` 和量化 `/32768` 的真实 `div`
指令；默认 unity gain 下每个 epoch 约执行 128 次。改为保持 C 向零截断语义的有符号移位，
为 unity gain 增加等价快路，并利用已确认 32 B 对齐的 USB VDMA ring 做两个 speaker int16 的
单次 32-bit 搬运。haptics peak/nonzero 在转换时增量计算，字段复用 epoch slot 尾部 padding。

验证结果：

- 全部 65,536 个 int16 输入乘以 8 组 gain（含 256 和 65,535）与旧 C 实现逐值位精确；
- 未对齐 host 输入回退路径与对齐路径输出一致；
- 目标反汇编中 `m61_audio_epoch_ingest_usb()` 从 `0x36a` 缩到 `0x326`，热函数无 `div/divu`；
- pipeline A/B ingress 平均从 `32 us/packet` 降到 `30 us/packet`，改善 6.25%；
- core-HPM 90 秒 encode 从 `4047 us / 1,296,022 cycles` 降到
  `3984 us / 1,275,350 cycles`，改善约 1.6%；P99 保持 5250 us，max 从 5919 us 降到
  5795 us；
- 静态 RAM 保持 197,124 B，所有 drop/deadline/cancel/stale/error 为 0。

### 7.6 正式 RAM 布局下的 decoder 并发基线

在 `pvq-mdct-clusters`、Opus 1.2.1 fixed、O2+LTO 和 ingress 除法消除均启用的 core-HPM
固件上，运行 90 秒 `baseline-v1` 并开启固定 71 B synthetic mic decoder：

| 指标 | 当前并发结果 |
| --- | ---: |
| encode 平均 | 4190 us / 1,341,702 cycles |
| decode 平均 | 2927 us / 935,870 cycles |
| encode instret | 212,577 |
| decode instret | 136,018 |
| encode I-cache miss | 2,809 |
| decode I-cache miss | 1,935 |
| codec cycles 合计 | 2,277,572 / 10 ms |
| 单核周期占比 | 71.2% |

相对旧 decode `4111 us / 1,313,852 cycles`，当前 decoder cycles 改善约 28.8%，证明正式
PVQ+MDCT/FFT cached OCRAM 布局同时帮助 decode。并发 encode 相对 encode-only 的
`1,275,350 cycles` 增加约 5.2%，量化了共享 I-cache/任务竞争成本。该轮 8,438 次 decode
错误为 0，所有 audio/BT drop、deadline、cancel 和 stale 均为 0。

mono encode+decode 尚有约 28.8% 原始周期空间，但该数字未包含真实 stereo encoder 增量、
真实 BT mic ingress/USB IN 和最坏协议突发，不能视为已满足最终 20% 冗余。

## 8. 代码级性能问题清单

### P0：必须先修的正确性与测量问题

1. `m61_audio_epoch_fallback_due_pair()` 在首 epoch 接近 32 ms deadline 且剩余不足 9 ms 时，
   把待编码 epoch 直接标记 complete、`speaker_enabled=0`，只保留 haptics。必须删除此主动
   丢 speaker 逻辑。过载时仍可记录 deadline miss，但不能伪装为成功完成。
2. HPM 只覆盖 codec 的 I-cache/D-cache read，不足以解释 ingress、resample、CRC、BT
   allocation/send 和任务抢占。必须增加阶段化端到端指标。
3. 必须核实 48 kHz USB descriptor、实际每 SOF 48/49 frame、512-frame epoch fill 周期、
   480-sample Opus、双 epoch BT report cadence 之间的真实时钟关系。记录长期 buffer phase 和
   report interval，排除周期性音色变化来自 clock drift/cadence beating。
4. 当前 profile/正式构建目录可能保存不同 RAM candidate。每次结果必须记录 Git commit、
   SDK commit、完整构建参数、ELF SHA256、`.itcm` size 和关键函数地址，禁止只按文件名判断。

### P1：USB ingress 热路径

`m61_audio_epoch_ingest_usb()` 当前逐 frame：

- 用四次 byte load 拼两个 speaker `int16_t`；
- 每 16 frame 处理一次 haptics；
- `pcm16_to_i8()` 对运行时 `gain_q8` 生成 `/256`，再执行 `*127/32768`；左右声道重复；
- 完成 epoch 后再次扫描 64 B haptics 计算 nonzero/peak。

实施要求：

1. 先给 ingress 独立增加 cycles/instret、D-cache read/write miss 和 wall-time 分布。
2. 对实际默认 `gain_q8==256` 建立专用路径；通用 gain 路径保留。
3. signed 常量除法用移位和向零修正替换时，必须覆盖全部输入值并逐值证明与 C 除法一致。
4. 在确认 USB VDMA ring 32 B 对齐、RISC-V 非对齐策略和 endian 后，用 aligned 32-bit load
   一次读取左右 speaker，再写入连续 PCM；haptics 可在 decimation frame 单独读取。
5. peak/nonzero 可在生成 haptics 时增量维护，避免第二次 64 B 扫描，但不得延长关中断区。
6. 单独 A/B `m61_audio_epoch.c` 的 `-Os` 与 `-O2`。只在 cycles、P99、I-cache 和 RAM 均
   通过时保留，不全工程改 O2。

### P2：decoder 与共享 Opus/CELT

在当前正式 PVQ+MDCT/FFT RAM 布局下重新运行 71 B mic decoder benchmark，旧的 1.314 M
cycles 不能继续当作新布局结果。新增 decoder 阶段至少包括 entropy/PVQ、inverse MDCT/FFT、
deemphasis 和 output copy，并记录 encode-only、decode-only、encode+decode 三组 cache 竞争。

后续共享底层优先级：

1. `opus_fft_impl`、MDCT forward/backward 的循环、访存和调用布局；
2. PVQ 的 `quant_partition`/`quant_band`/`compute_theta` 等紧密簇内部；
3. decoder 的 `decode_pulses` 和 entropy hot loop；
4. 只有 HPM 证明 miss-heavy 且连续调用关系明确时才调整 OCRAM placement。

所有新 DSP 指令候选先做边界值、100 万随机值、1200-frame encoder stream 和至少 24 万
decoder checksum。随后做两轮真机 A/B；只减少 instret 但 cycles 不降的候选不得保留。

### P3：512→480 重采样

当前 mono 线性函数每个 output sample 重复计算位置、除法、左右平均、插值和 `/480`。
512:480 是精确 16:15 比例，可改为 15 相 phase accumulator/polyphase，实现时：

1. 第一版只做数学等价的位精确线性重排，输出逐样本与当前实现一致，用于隔离循环/除法收益。
2. 第二版才评估高质量 band-limited fixed-point polyphase，并以外部 WDL/协议参考为质量基线。
3. 终态必须是 stereo-in/stereo-out，不允许先 downmix；mono 版本只用于当前过渡基线。
4. 质量验收覆盖 20 Hz–20 kHz sweep、单频、双频、脉冲、白噪声、左右独立和反相信号；
   passband、alias、THD+N 和 SNR 不得劣于参考实现的测量误差范围。
5. cadence 审计未完成前，不得通过改变 512/480 比例“修音色”。

### P4：bridge 事件驱动与 codec 让步

将 bridge 从 1 ms polling 改为 task notification/event bits，事件来源包括：

- 新 epoch pair ready；
- USB HID output/feature request；
- BT TX completion 或 net_buf 可用；
- link generation/change；
- mic status deadline；
- 显式周期 timer，仅用于真正的协议 deadline。

必须保留 `CONFIG_M61_BRIDGE_MODE_POLLING` 作为 A/B。事件模式在固定全负载下仍必须发送全部
audio/haptics/HID 数据，不能合并掉协议要求的实时包。比较 wakeups/s、bridge runtime、
I/D-cache miss、BT send latency 和 codec P99。

codec 末尾固定 1 tick delay 改为 deadline/event 驱动：每次最多处理一个到期 frame，随后
若 bridge/BT 有待发送事件则主动让步；若无事件，可等待下一个 epoch notification。禁止恢复
无界 catch-up loop，因为历史上它会饿死 bridge 并造成约 50 ms haptics 卡顿。

### P5：报告构造、CRC 与 Bluetooth buffer

`dualsense_output.c` 当前对 547 B realtime report 使用逐 byte、逐 bit CRC32：每包约 4376
次 inner iteration。SDK 已有 256-entry CRC32 表实现。

1. 先单独测 `dualsense_output_make_audio_rt()`、memset/memcpy、CRC 和 L2CAP allocation/send。
2. 用 1 KiB table CRC 替换时，对 report31/32/36/39 和 feature seed 的现有向量逐包证明 CRC
   完全一致；表的 I-cache/D-cache 成本必须计入真机 A/B。
3. 硬件 CKS 只提供 16-bit checksum，不适用。安全引擎 CRC32 对 547 B 的启动和总线开销
   未知，只能作为后续 A/B，不作为默认方案。
4. 记录 `bt_l2cap_create_pdu_timeout(K_NO_WAIT)` 的 allocation cycles、失败码、pool high-water、
   enqueue 到 completion 延迟。当前 `CONFIG_BLE_TX_BUFF_DATA=4` 通常对应四个 L2CAP TX buffer；
   只有实际出现 `-ENOMEM/-EAGAIN` 才增加数量。
5. 增加 buffer 是 RAM 换抖动，不是 CPU 优化；不得用大队列掩盖生产速度长期小于消费速度。

### P6：热点文件编译优化

除源码 Opus 外，应用和 USB/SDK C 文件当前主要使用 `-Os`。按以下顺序逐文件 A/B `-O2`：

1. `m61_audio_epoch.c`；
2. `m61_usb_gamepad.c`；
3. `dualsense_output.c`；
4. 若 profile 证明 USB driver 占比明显，再测试 `bflb_usb_v2.c`。

每次只改一个 translation unit。记录 text、目标函数大小、I-cache miss、cycles、P99 和 RAM。
全应用 O2、全 SDK O2、全库 O3 均不允许直接合入。

### P7：内存容量整理

当前主要静态余量：

- encoder state 预留 49,152 B，实际约 38,436 B；
- decoder state 预留 24,576 B，实际约 17,776 B；
- codec stack 32,768 B，历史 HWM 仍空闲约 20,448 B；
- audio epoch store 约 9,432 B；
- USB non-cache rings 约 7.8 KiB。

收缩 state reserve 和 stack 不会直接降低 codec cycles，但可为 stereo encoder state、双声道
PCM、额外 profile 和必要 BT buffer 留出片上 RAM。必须用 `opus_*_get_size()`、stack HWM
和至少一小时压力测试确定余量，不按一次观测值贴边缩小。

## 9. 后期硬件实验

以下项目只有在 P0–P6 完成后才进入：

### 9.1 总线 arbitration/QoS

- 启动后只读并记录 `BMX_CFG0/1` 原值。
- 分别 A/B arbitration mode、QOS_CPU、QOS_SDU、QOS_BLEM；一次只改一位。
- 同时测 codec cycles、USB ingress gap、BT HCI/ACL error、I/D-cache 和总线超时状态。
- 任意 USB/BT 稳定性回归立即否决。不得把未知位组合写入默认固件。

### 9.2 USB selective FIFO

- 只为 audio OUT EP1 设计双 FIFO；保持 EP2/EP3/EP4 单 FIFO。
- 验证 enumeration、音频 OUT/IN、HID OUT/IN、suspend/reset 和 VDMA error path。
- 目标是降低 ingress jitter/ISR re-arm 压力，不预期降低 Opus cycles。

### 9.3 pSRAM

先只初始化和微基准，不迁对象。测试：4 KiB/40 KiB 顺序读写、随机 32 B/64 B、cold/warm
cache、CPU 与 USB/BT 并发。若稳定，再依次迁移冷日志、诊断表、非实时 feature cache 或大型
测试数据。热 Opus state、epoch、BT scheduler 和 USB VDMA ring 不迁 pSRAM。

### 9.4 USB HS

仅做物理链路/枚举/稳定性实验。HS 对 host packet scheduling 或抖动可能有帮助，但 USB audio
当前带宽远低于 FS 上限，不能把 HS 当作性能冗余的主要来源。

## 10. 测量规范

### 10.1 两套固定场景

每个候选至少运行两种场景，禁止跨场景比较：

1. transport baseline：手柄不动、不主动播放听感音频，但 Windows 保持 Audio OUT 打开，
   USB 持续发送零 PCM；用于与早期历史基线比较。
2. deterministic full load：`run_m61_full_load.py` 默认参数，speaker 440/733 Hz、左右 haptics
   160/223 Hz、每 20 ms 变化一次 HID SetState；用于最终容量验收。脚本默认参数必须固定，
   命令行不指定参数时直接运行标准场景。

每轮正式比较至少 90 秒、两轮有效结果。断连、手柄没电、外部断电、`hidp_audio sent=0` 或
stale 大幅增长的轮次整体作废，不从中选择好看的局部区间。

### 10.2 必须记录的指标

- 每阶段 wall-time：last/P50/P95/P99/max；
- cycles、instret、CPI；
- I-cache access/miss；
- branch/mispredict；
- D-cache read/write access/miss；
- ingress VDMA completion→task age、epoch fill interval 和最大关中断 cycles；
- resample、encode、decode、report build、CRC、net_buf alloc、L2CAP send 各阶段；
- bridge wakeups/runtime、codec/bridge/BT RX/TX task runtime 和抢占时间；
- USB ring high-water/drop/gap；
- epoch qdrop/cancel/deadline/discontinuity；
- mic underflow/drop/decode error；
- BT realtime stale/retry/drop、`-ENOMEM/-EAGAIN`、send completion latency；
- stack HWM、heap current/min、各静态 section 大小和关键热函数地址。

HPM counter 数量不足时按固定 profile 组分多次运行。不同 profile 组必须使用相同源提交和负载，
profile 宏只加到应用目标，禁止再次传播到 RF/PHY/BT 驱动。

### 10.3 单项优化通过门槛

候选只有同时满足以下条件才保留：

1. 两轮加权 cycles 至少改善 1%，或 P99 至少改善 0.25 ms；低于此值视为噪声，除非同时
   明显减少 drop/underflow。
2. P99、max、I-cache miss 和任一实时错误不得稳定恶化。
3. 功能测试、协议向量、USB 枚举、Steam/DSX 输入、speaker/haptics 听感全部通过。
4. 位精确候选通过 stream/checksum；非位精确候选通过客观质量和双盲/可重复听感。
5. 记录最终 ELF SHA256、构建参数和 Git commit。

平均值改善但 P99、drop、deadline、stale、underflow 或音质恶化时，一律判定失败。

## 11. 分阶段实施路线

### 阶段 0：冻结可复现基线

- 正式默认固定 Opus 1.2.1 fixed、O2+LTO、E907 patch、preload+AMR、
  `pvq-mdct-clusters`。
- 清理构建配置残留，确认 ELF 不包含 energy/TF candidate。
- 在 manifest 中记录项目/SDK commit、工具链版本、构建参数、SHA、section 和热函数地址。
- 运行 transport 和 deterministic full load 各两轮。

完成门槛：结果复现本规格 3.971 ms/1.273 M cycles 基线的合理波动，实时错误为 0。

### 阶段 1：端到端 profile 与 fallback 删除

- 增加 cadence、ingress、resample、CRC、BT alloc/send、task runtime 指标。
- 删除 `m61_audio_epoch_fallback_due_pair()` 的 speaker 取消行为及对应成功伪装。
- 保留 fault counter；若过载，应明确暴露 deadline miss，而不是静默停 speaker。

完成门槛：固定满载 0 drop/0 deadline/0 stale。全链路 profile 允许作为重型诊断构建存在，
但 core HPM 与生产构建必须完全编译掉其热路径代码；不得再要求重型 profile 自身低于 2%。

### 阶段 2：ingress 位精确优化与局部 O2

- 默认 gain 快路、常量除法、aligned deinterleave、增量 peak/nonzero。
- `m61_audio_epoch.c` 做 `-Os`/`-O2` A/B。

完成门槛：USB PCM、haptics byte stream 位精确；ingress cycles/P99 明确下降。

### 阶段 3：正式 decoder 基线与 Opus 共享热点

- 重测 decode-only 和 encode+decode；补齐 decoder stage HPM。
- 继续 FFT/MDCT/PVQ 内部位精确 E907 优化，不继续猜测搬小函数。

完成门槛：mono encode+decode 的全链路 P99 不超过 8 ms，实时错误为 0；否则不进入 stereo。

### 阶段 4：cadence 与高质量 stereo resampler

- 先完成 512/480 时钟关系证明。
- 实现位精确 15-phase 线性版本，再评估 band-limited stereo fixed-point 版本。

完成门槛：质量不劣于参考；resample CPU 明确下降；长期无 buffer phase 漂移和音色调制。

### 阶段 5：bridge/codec 事件调度

- 实现 event-driven bridge 和 deadline-aware codec 让步，保留 polling A/B。
- 不改变满载发送量，不依赖静音/无输入。

完成门槛：满载 bridge/BT latency 和 cache 指标改善，Steam/DSX 输入无回归。

### 阶段 6：CRC、报告构造和 BT buffer

- table CRC 位精确 A/B；测量 allocation/send completion；按证据调整 buffer count。

完成门槛：CRC 向量全过，realtime `-ENOMEM/-EAGAIN/stale` 为 0，cycles/P99 改善。

### 阶段 7：硬件实验

- QoS/arbitration、selective USB FIFO、pSRAM 冷数据、USB HS 按独立分支逐项 A/B。

完成门槛：只有真机端到端收益且无稳定性回归的实验进入默认配置。

### 阶段 8：真实双声道 + mic 全并发

按顺序恢复：

1. speaker stereo、160 kbps、参考带宽，mic 仍关闭；
2. stereo speaker + synthetic 71 B decoder；
3. stereo speaker + 真实 BT mic + USB IN；
4. 同时加入 HD haptics、最大频率 HID output、输入和 feature request。

最终硬门槛：

- speaker-only P99 ≤ 7 ms；
- stereo speaker + mic decode + USB/BT/调度的最坏稳定 P99 ≤ 8 ms/10 ms；
- 至少 20% 周期余量；
- 所有 audio/haptics/USB/BT drop、fallback、stale、underflow、codec error 为 0；
- 连续一小时压力测试无断连、输入冻结、周期性音色变化或震动卡顿；
- Steam 和 DSX 均识别并响应全部输入；
- speaker 主观和客观质量不低于参考 USB/协议实现。

如果真实 stereo+mic 在完成上述底层优化后仍无法达到 20% 余量，应明确得出“BL616 单核
容量不足”的工程结论，再评估双核/更高性能 MCU 或外置 codec。不得用降低质量、暂停功能
或隐藏 drop 来宣称完成。

## 12. 构建、刷写和 Git 规范

### 12.1 Windows 原生与 WSL 构建

Windows 原生构建使用 SDK 自带 CMake、Ninja、`mingw32-make` 和 Xuantie GCC 10.2.0。
Opus 1.2.1 必须由 Windows 工具链重新生成 O2/LTO archive；Linux 生成的 GCC LTO IR 不能
交给 Windows `lto1.exe` 链接。推荐入口：

```powershell
cd C:\code\MCU\DS5Dongle_ref\m61\dualsense_hidp_probe
.\build_windows.ps1 -Command All
.\build_windows.ps1 -Command All -HpmProfile
```

`-RebuildOpus` 仅用于需要从头验证 Opus archive 的场景。Windows 构建使用独立
`build-win/`，不得与 WSL 的 `build/` 互相复用。2026-07-14 静态验证结果：clean Opus +
clean SDK profile build 为 58.12 s，`.itcm=27176 B`，realtime RAM gate 通过；该宿主生成
固件在替换正式性能基线前仍必须完成真机 HPM 对照。

WSL 继续作为回退和交叉核验路径。避免在 `/mnt/c` 做频繁 clean build；SDK 的大量小文件
和元数据扫描会受到挂载层显著影响。WSL 推荐入口：

```bash
cd /mnt/c/code/MCU/DS5Dongle_ref/m61/dualsense_hidp_probe
./build.sh clean
./build.sh build --opus-source-o2-lto --opus-tcm-profile pvq-mdct-clusters
```

WSL ext4 构建完成后，禁止临时拼接包含 `$src/$dst` 的 PowerShell→`bash -lc` 命令，也禁止
复制整个含 SDK 中间文件的 `build_out`。统一从 Windows 主工作区运行：

```powershell
.\sync_wsl_artifacts.ps1 -ExpectedProfile Core
```

脚本只复制刷写所需的 9 个固件/ELF/map 产物，检查 core/pipeline 编译开关，并逐文件核对
SHA256。pipeline 诊断构建使用 `-ExpectedProfile Pipeline`，生产构建使用 `Production`。

无论使用哪个宿主，profile 都必须使用独立构建状态或完整 clean，显式加入需要的 profile
flag。构建后运行：

- ELF/section/关键符号检查；
- `check_m61_realtime_memory.py`；
- 协议和 host 单元测试；
- 固件 manifest/SHA256 生成。

### 12.2 刷写

- 用户进入下载模式后刷写新固件；不要无意义重编译旧版本。
- 刷写完成后等待用户人工 reset，或让 USB 口重新上电；刷写工具不得假设板子会自动启动。
- reset 后先确认初始化红灯熄灭、蓝牙状态灯逻辑、串口启动完成和 firmware SHA，再开始测试。

### 12.3 Git

- 每个有效优化、可复现实验设施或明确否决结论单独提交。
- 提交前运行 `git diff --check` 和相关 offline/host 测试。
- 不提交四份原厂 PDF、`artifacts/`、构建缓存、日志和第三方下载源。
- commit message 使用 `perf(m61):`、`test(m61):`、`docs(m61):` 等明确前缀。

## 13. 依据索引

原厂资料：

- `docs/ai-m61-32s-kit_v1.1.2_product_specification_cn.pdf`：已查看 PDF viewer 第 4、10、14
  页的模组资源、开发板接口和引脚图片；
- `docs/bl616_bl618_ds_zh_cn_2.5_open_.pdf`：已查看 PDF viewer 第 13、14、17 页的 E907、
  cache、总线和存储框图；
- `docs/bl616_bl618_rm_zh_cn_0.98_open_.pdf`：已查看 PDF viewer 第 41、42、176–181、
  384–385 页的系统总线、存储器和 USB 图片/寄存器说明；
- `docs/nodemcu-ai-m61-32s-kit_v1.1.pdf`：已查看整页原理图，核对 BOOT、RGB、Flash 和
  pSRAM 连接。

以上页码是 PDF viewer 页码，不与文档正文印刷页码混用。

项目源码：

- `m61/dualsense_hidp_probe/m61_audio_epoch.c`；
- `m61/dualsense_hidp_probe/m61_usb_gamepad.c`；
- `m61/dualsense_hidp_probe/main.c`；
- `m61/dualsense_hidp_probe/dualsense_output.c`；
- `m61/dualsense_hidp_probe/m61_bt_tx_scheduler.c`；
- `m61/dualsense_hidp_probe/build.sh`、`build_opus.sh` 和 `patches/`；
- `tools/run_m61_full_load.py`、`compare_m61_perf_status.py` 和协议/内存检查工具。

SDK 源码：

- `bsp/board/bl616dk/bl616_common.ld.in`：OCRAM/WRAM/pSRAM 和 `.itcm` 链接布局；
- `bsp/board/bl616dk/board.c`、`board_flash_psram.c`：CPU、USB、Flash 和 pSRAM 初始化；
- `drivers/soc/bl616/std/startup/system_bl616.c`：PMP、MXSTATUS 和启动；
- `drivers/sys/bl616/bl616_sys.c`：sysmap、cache attributes、preload 和 AMR；
- `drivers/lhal/src/bflb_usb_v2.c`：USB FIFO mapping 和 VDMA；
- `drivers/lhal/include/arch/risc-v/t-head/rv_hpm.h`：可用 HPM 事件；
- `drivers/soc/bl616/std/include/hardware/glb_reg.h`：BMX arbitration/QoS；
- Bluetooth L2CAP/net_buf 配置：`CONFIG_BT_L2CAP_TX_MTU`、`CONFIG_BLE_TX_BUFF_DATA`。

外部参考仅用于交叉核对协议和数据流：

- `egormanga/SAxense`；
- `awalol/DS5Dongle`；
- `controllers.fandom.com/wiki/Sony_DualSense#USB_2`。

## 14. 立即执行顺序

下一次代码工作从阶段 0/1 开始，不再继续试搬 RAM：

1. 确认正式 ELF 为 `pvq-mdct-clusters`，排除 TF candidate 配置残留；
2. 补 ingress/resample/report/CRC/BT/task/cadence 指标；
3. 删除 speaker deadline fallback；
4. 以正式默认脚本跑两轮新基线；
5. 实施 ingress 位精确优化；
6. 在当前 RAM 布局下重测 decoder，随后决定 FFT/MDCT/PVQ 的下一处底层优化。

这一路线优先增加真实满载上限，不以工程外观重构、空闲省算力或质量退让替代性能优化。
