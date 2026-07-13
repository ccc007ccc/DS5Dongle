# M61 内存路径与性能优化规格

## 1. 目标与约束

本规格只依据 Ai-M61-32S-Kit/BL616/BL618 原厂资料、当前 SDK 源码、当前
ELF/启动日志和真机 HPM 指标。项目历史文档只能作为问题线索，不能作为硬件事实。

目标是在不降低音质、不减少功能负载、不依赖空闲时省算力的前提下，提高扬声器、
震动和未来麦克风并发时的性能上限。所有内存放置和 cache 调整必须进行同负载真机
A/B，不能按“RAM 理论上更快”直接合入。

## 2. 已核实的硬件事实

1. Ai-M61-32S 模组使用 BL618，原厂模组规格标明 4 MB pSRAM、532 KB SRAM、
   128 KB ROM；开发板启动日志进一步报告 `QFN56`、`WB_4MB` pSRAM、8 MB 外置
   Flash。
2. CPU 是 E907，最高且当前工作频率均为 320 MHz。原厂数据手册给出的系统总线
   频率为 80 MHz；当前 SDK 也把 CPU 配到 320 MHz，把外置 Flash 校准到 80 MHz。
   因此当前没有文档内的 CPU 超频余量。
3. E907 有 32 KB、两路组相连 I-cache 和 16 KB、两路组相连 D-cache。SDK 的
   RV32 cache 操作按 32 字节 cache line 工作。
4. CPU 有 AXI 和 AHB 两条访问路径。Flash、ROM、pSRAM 位于 AXI 侧；USB、DMA、
   无线模块和片上 SRAM 位于 AHB 侧。CPU 对片上 SRAM 的 cache 地址访问需要经过
   cache、AXI 到 AHB 的桥接。

## 3. 实际地址和访问路径

| 存储区域 | Cache 地址 | Non-cache 地址 | 当前用途与结论 |
| --- | --- | --- | --- |
| OCRAM 320 KB | `0x62FC0000` | `0x22FC0000` | cache 地址经 AXI→AHB；non-cache 地址直连 AHB |
| WRAM 160 KB | `0x63010000` | `0x23010000` | 与 OCRAM 相同的双别名机制；一部分由蓝牙 EM/共享 RAM 保留 |
| Flash XIP | `0xA0000000` | 无 | 80 MHz QIO、continuous read、burst wrap，代码默认从这里执行 |
| pSRAM 4 MB | `0xA8000000` | 无公开 non-cache 别名 | 硬件存在，但当前固件没有启用 `CONFIG_PSRAM` |

原厂手册把 OCRAM/WRAM 描述为片上零延迟 SRAM，但这不表示 SDK 的任意 RAM 段都
由独立 TCM 端口取指。手册对 `0x62FC0000` 的路径有更具体说明：访问经过内部 cache，
再通过 AXI 转 AHB 到 OCRAM。评价性能必须以这个具体路径和真机计数器为准。

## 4. SDK 链接脚本的关键误区

BL616DK 链接脚本中的 `.itcm` 和 `.dtcm` 只是输出段名称：

- `ram` 的起始地址是 `0x62FC0000 + 0x400`；
- `.itcm`、`.dtcm`、`.data`、`.bss` 和 heap 都连续放在这个 `ram` 区域；
- `.tcm_code.*` 被收集到 `.itcm`，但最终仍是 cacheable OCRAM/WRAM；
- 当前稳定 ELF 的 `.itcm` 约 10.4 KB，Opus 热函数仍位于 `0xA001xxxx` Flash XIP。

因此，“给函数加 TCM section”在本项目中的真实含义是“开机时从 Flash 复制到
cacheable OCRAM”，不是进入独立、零等待的硬件 ITCM。此前把约 39.6 KB Opus 热代码
整体放入该段后真机明显退化，已经足以否决大块整体搬迁策略。

## 5. 为什么 RAM 代码可能反而变慢

1. I-cache 命中时，Flash XIP 代码和 cacheable OCRAM 代码都由 I-cache 向 CPU 供给，
   搬迁本身不能改善命中路径。
2. I-cache miss 时，OCRAM 需要走 AXI→AHB 和 80 MHz 系统总线；Flash 则使用已经
   校准的 80 MHz QIO、continuous read 和 burst wrap。不能只按介质名称推断 line fill
   的实际代价。
3. E907 I-cache 只有 32 KB、两路组相连。一次搬入几十 KB 会改变全部函数地址、cache
   set 映射和调用布局，可能增加冲突 miss；代码总尺寸变小也不保证热点 working set
   更合适。
4. RAM 段同时承载状态、堆、任务栈和音频 epoch。整体搬代码会减少片上 SRAM 余量，
   但不会扩大 I-cache。

所以 RAM 仍可能提升性能，但只适合单个已证实存在 miss/布局问题的小热点，且必须逐个
函数 A/B。禁止再次整体搬迁 libopus 或大批热函数。

## 6. 数据路径结论

USB DMA 的 endpoint buffer 位于 `0x22...` non-cache 区是正确的，避免 CPU cache 与
DMA 不一致。当前音频路径只从 DMA ring 读取一次，随后把 PCM 和 epoch 状态复制到普通
cacheable SRAM；不应把 Opus state、PCM epoch 或编码工作区迁到 non-cache 区。

4 MB pSRAM 可以提高容量上限，但不能直接提高 codec 算力。优先用途是未来的大型冷缓冲、
日志、非实时资源或低频访问状态，以便给片上 SRAM 留出余量。Opus state、当前/下一帧
PCM、USB/BT 调度状态不得在没有真机证据时放入 pSRAM。

## 7. 后续真机实验优先级

### P0：D-cache preload/AMR

SDK 提供 `bl_cpu_sysmap_init(dcache_preload_en, dcache_amr_en)`，可以配置正确的 memory
attributes，并分别启用两行 D-cache preload 和三行 Adaptive Miss Handling for Writes。
当前项目没有调用该函数。

按以下四组独立固件测试，其他代码和负载完全不变：

1. preload=0，AMR=0；
2. preload=1，AMR=0；
3. preload=0，AMR=1；
4. preload=1，AMR=1。

记录 encode us/cycles/instret、I/D-cache access/miss、P50/P95/P99、USB ingress age、
queue drop、deadline、BT stale 和最大关中断周期。只有 cycles、P99 和实时错误同时不
恶化的组合可以保留。

2026-07-13 首个 `preload=1, AMR=1` 候选已完成两轮 baseline-v1 真机负载：

- 第一轮：5000 us / 1,602,328 cycles / 223,942 instret；
- 第二轮：5048 us / 1,617,322 cycles / 225,344 instret；
- 两轮共 16,874 帧，累计平均 5024 us / 1,609,825 cycles / 224,643 instret；
- 相对已提交的 Opus 1.2.1 E907 基线 5287 us / 1,693,169 cycles / 226,993
  instret，平均时间和 cycles 均降低约 5.0%；
- P99 保持 6250 us，queue drop、deadline、BT stale 和编码错误均为 0；
- 两轮累计 D-cache read miss 约 761/encode。

组合配置已达到保留门槛，但仍需分别测试 preload-only 与 AMR-only，确认收益来源并避免
保留无效或负收益配置位。

拆分 A/B 随后完成：

- preload-only 两轮平均约 5103 us / 1,634,573 cycles / 225,558 instret，D-cache
  read miss 约 758/encode；
- AMR-only 两轮平均约 5107 us / 1,635,953 cycles / 225,281 instret，D-cache
  read miss 约 1006/encode；
- 两个单项分别都比旧基线快约 3%，但都稳定慢于 combined 约 1.5%；
- preload 主要降低 read miss，AMR 在 write miss 处理上提供额外收益，两者互补。

最终决策是同时保留 preload 和 AMR；单项固件只作为实验归档，不进入默认配置。

### P1：严格的 Flash 与 OCRAM 取指微基准

测试固件只在 `CONFIG_M61_MEMORY_BENCH` 启用时链接两个无重定位汇编内核。Flash 中只
保留一份机器码，运行时把完全相同的 4096/40960 字节复制到 32 字节对齐的 cached
OCRAM，clean D-cache 并 invalidate I-cache 后通过函数指针执行。每组校验源/副本字节和
返回 checksum；计时窗口进入短临界区，防止较慢的 XIP 样本把蓝牙/USB 中断指令计入
`minstret`。测试配置不进入正式固件。

2026-07-14 两轮 best-of-7 真机结果高度一致：

| 工作集 | 状态 | XIP cycles | OCRAM cycles | OCRAM/XIP | instret |
| --- | --- | ---: | ---: | ---: | ---: |
| 4 KiB | cold | 48023–48026 | 10197–10200 | 21.2% | 两者均 1155 |
| 4 KiB | prewarmed | 1198 | 1215–1217 | 101.4–101.6% | 两者均 1155 |
| 40 KiB | cold | 474026 | 94688–94692 | 20.0% | 两者均 10371 |
| 40 KiB | prewarmed | 292996–293002 | 62256 | 21.2% | 两者均 10371 |

结论：

- I-cache 全命中时，XIP 与 cached OCRAM 基本等价，OCRAM 本轮还慢约 1.4%；把已经能
  驻留 cache 的小热点搬 RAM 没有收益。
- 发生 line refill 时，cached OCRAM 明显快于 80 MHz QIO XIP；按 4 KiB cold 与 warm
  差值估算，XIP 每个 miss 约增加 363 cycles，OCRAM 约增加 69 cycles。
- 40 KiB 超过 32 KiB I-cache 后，两种位置的 miss 数几乎相同，但 OCRAM 总 cycles
  只有 XIP 的约 20–21%。RAM 不会消除 I-cache miss，只会降低 refill 代价。
- 这证明“选择性放 RAM”有真实硬件潜力，但不推翻此前整体搬约 39.6 KiB Opus 的负面
  真机结果。整体搬迁同时改变函数布局、cache set 冲突和 SRAM 压力；下一步只能逐个
  高频且 miss-heavy 的函数 A/B。

### P2：选择性函数放置

仅对 HPM/反汇编确认的单个热点尝试 OCRAM 放置；每次只移动一个函数或一个紧密调用簇。
若 I-cache miss、cycles 或 P99 任一稳定恶化，立即回退。代码大小或理论延迟不能作为
通过依据。

当前正式编码窗口平均约有 3146 次 I-cache miss/帧。微基准给出的 refill 差值意味着
理论上存在明显上限，但不是每个 miss 都来自可安全迁移的 Opus 函数。候选顺序按“小步、
每帧必经、RAM 成本可控”排列：先单测约 3.7 KiB 的 `quant_all_bands`，再视结果测试
`compute_allocation` 或紧密的 band quantization 调用簇；不从约 14.8 KiB 的
`celt_encode_with_ec` 或整个 CELT 编码器开始。

### P3：启用并测量 pSRAM

先只初始化 pSRAM并做带宽、随机延迟和 cache miss 测试，不移动实时对象。确认启动、
低功耗恢复和蓝牙共存稳定后，再评估把冷数据移出片上 SRAM；pSRAM 不作为 Opus 热代码
或热状态的默认目标。

## 8. 当前决策

- 保留 Opus 1.2.1 fixed 和已验证的位精确 E907 Q15/Q16 等优化。
- 不再测试 Opus 1.6.1，也不再整体搬迁 Opus 到 SDK 所谓 `.itcm`。
- 当前最有实际依据的底层性能实验是 D-cache preload/AMR 四组合。
- pSRAM 是容量工具，不是未经测量的性能工具。
- CPU 已在原厂 320 MHz 上限，Flash 已在 80 MHz QIO 优化模式；不以超频作为方案。

## 9. 原始依据

- `docs/ai-m61-32s-kit_v1.1.2_product_specification_cn.pdf`：第 4–6 页。
- `docs/bl616_bl618_ds_zh_cn_2.5_open_.pdf`：第 12–14、70 页。
- `docs/bl616_bl618_rm_zh_cn_0.98_open_.pdf`：第 29–34 页。
- `docs/nodemcu-ai-m61-32s-kit_v1.1.pdf`：模组与开发板原理图。
- `bl_mcu_sdk/bsp/board/bl616dk/bl616_common.ld.in`：RAM/ITCM/DTCM 链接布局。
- `bl_mcu_sdk/drivers/soc/bl616/std/startup/system_bl616.c`：cache 启用和 PMP 区域。
- `bl_mcu_sdk/drivers/sys/bl616/bl616_sys.c`：memory attributes、preload 和 AMR。
- `bl_mcu_sdk/bsp/board/bl616dk/board.c`、`board_flash_psram.c`：320 MHz CPU、80 MHz
  QIO Flash 和 pSRAM 初始化能力。
