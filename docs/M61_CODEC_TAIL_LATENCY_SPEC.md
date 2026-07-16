# M61 DualSense Codec 尾延迟优化执行规范

状态：执行中  
版本：1.0  
日期：2026-07-16  
目标平台：Ai-M61-32S / BL618-class 模组，Bouffalo BL616 兼容 SDK 目标  
项目基线：`9b3f8f3`（DualSense D4/71 B decoder parser 快路）  
SDK 基线：`d9306a4a`

## 1. 目标

本规范只解决真实全双工负载下的 codec、USB 和 Bluetooth 尾延迟。优化不得依赖静音、
减少测试负载或降低音频质量。

阶段目标：

1. Encode + Decode 平均耗时由约 `7.16 ms` 降至 `6.2-6.6 ms`。
2. 同一 10 ms 调度窗口内 codec critical-path P99 降至 `8.5-9.0 ms`。
3. 为 USB、Bluetooth controller/host、HID 和 RTOS 保留至少约 `1 ms` 的 P99 预算。
4. speaker/haptics epoch drop、deadline、cancel、codec error 全为零。
5. mic Opus/PCM drop、BT stale/retry/reject/send error、运行期 USB IN underflow全为零。
6. mic queue age max 优先降至 `10 ms` 以下；无法达到时必须能由 outlier 记录解释原因。

## 2. 不可改变的质量与负载条件

- 48 kHz、当前声道数、帧长、码率、带宽、CBR/VBR模式和complexity保持不变。
- deterministic full-load脚本的speaker、haptics、HID和mic负载保持默认值。
- 不允许通过丢帧、合并协议要求的数据包、关闭mic或降低haptics更新频率换取性能。
- Opus专用快路优先要求bit-exact；不能bit-exact的候选必须证明客观质量不劣化，且不得
  改变当前codec配置。
- 缓冲区只能吸收有界传输抖动，不能掩盖长期生产速度低于消费速度。

## 3. 当前基线

`9b3f8f3`第二轮正式90秒结果：

| 指标 | Encode | Decode |
| --- | ---: | ---: |
| 平均延迟 | 3,793.924 us | 3,367.501 us |
| 平均cycles | 1,214,105 | 1,075,832 |
| P50/P95/P99 | 4,250/5,000/5,250 us | 3,250/4,250/4,750 us |
| max | 5,938 us | 5,010 us |

平均codec合计约 `2,289,937 cycles/10 ms`，约占320 MHz单核预算的71.56%。当前独立
Encode/Decode P99相加恰好为10 ms，但独立percentile不能代表同一epoch，因此阶段0必须新增
rolling 10 ms和同一调度窗口的合计指标。

## 4. 已核实的硬件约束

- CPU公开上限320 MHz；系统总线公开上限80 MHz。
- I-cache为32 KiB、2-way、32 B cache line，共512 sets；同set地址周期为16 KiB。
- D-cache为16 KiB、2-way。
- OCRAM为320 KiB，WRAM为160 KiB；Bluetooth默认使用32 KiB WRAM作为EM。
- `0x62FC.../0x6301...`为cached alias，经Cache和AXI到AHB；`0x22FC.../0x2301...`
  为non-cache AHB alias。
- SDK链接段`.itcm`实际位于cached OCRAM，不是独立零等待TCM。
- 4 MiB pSRAM只用于冷数据、日志和大型非实时对象；热Opus code/state和USB/BT实时ring
  默认不得迁入pSRAM。
- BL618 Audio ADC/DAC不能执行Opus/CELT，本项目不使用板载模拟音频链路。
- USB具有VDMA和可配置共享FIFO；它能降低搬运/重装抖动，但不会减少Opus算法cycles。

## 5. 测量模型

### 5.1 正式构建

正式排名固件只保留：

- 每个codec阶段开始/结束各一次32位`mcycle`读取；
- Encode、Decode和combined-window histogram；
- queue depth/age、drop、stale、underflow等硬错误；
- 仅在超过阈值时写入固定大小outlier ring，不在热路径打印。

正式构建不逐帧读取instret和I/D-cache HPM，不维护每次锁的cycles统计。

### 5.2 诊断构建

HPM诊断按固定1/8或1/16采样率读取instret、I-cache和D-cache计数。采样帧和普通帧分开
累计，禁止用采样固件平均值直接替代正式排名。

### 5.3 Outlier flight recorder

触发条件初始设为：

- Encode >= 4,750 us；
- Decode >= 4,250 us；
- 同一10 ms窗口codec busy >= 8,500 us；
- mic queue age >= 10,000 us。

每条记录只保存时间、阶段、elapsed cycles、ready bits、codec/BT/USB队列深度、最近USB事件、
BT realtime待发标志和调度选择原因。状态任务低频导出，codec任务不得printf。

## 6. 分阶段路线

### T0：轻量计时和合计尾延迟

实现：

1. 用32位`mcycle`替代codec热路径的64位mtimer读取。
2. 固定320 cycles/us换算；后续超频实验从运行时CPU频率获取换算值。
3. HPM改为抽样；wall histogram每帧保留。
4. 新增rolling 10 ms codec busy和combined-window P50/P95/P99/max。
5. 新增outlier ring及延迟导出。

通过条件：协议/主机测试通过；统计样本一致；正式构建text/RAM增长可解释；两轮90秒无硬错误；
P99/max不稳定恶化。若cycles改善不足1%但P99改善至少0.25 ms，仍可保留。

### T1：ready-bit + absolute-deadline codec执行器

实现：

1. producer只发布`ENCODE_READY`、`DECODE_READY`和绝对deadline。
2. 继续使用单codec task，避免双任务切换和工作集互相污染。
3. 按`deadline - predicted_cost - now`的最小slack选择任务。
4. 每次最多完成一个重型stage；BT realtime待发时给bridge确定窗口。
5. 第二个codec任务只有在不破坏两个deadline时才允许同轮执行。
6. 空闲时阻塞到notification或最近绝对deadline，移除固定`vTaskDelay(1/2)`。
7. 保留当前polling/window策略作为编译期A/B回退。

禁止只用`ulTaskNotifyTake(portMAX_DELAY)`替代delay；ready状态和notification必须分离，通知丢失
或合并后仍能从ready bits恢复。

### T2：固定配置Opus encoder专用入口

固定条件为48 kHz、480 samples、mono、CELT、当前码率/CBR/complexity。将通用模式切换、
SILK/hybrid、可变帧长和动态声道冷分支拆离热路径；不满足条件时立即调用upstream通用入口。

通过条件：固定PCM corpus逐帧Opus长度和payload完全相同；随机/边界输入stream checksum一致；
真机Encode cycles至少改善1%且P99/max不恶化。

### T3：D4 decoder深层专用入口

在现有`TOC=0xD4`、71 B parser快路后，为48 kHz、480 samples、stereo、无FEC建立受保护的
单帧CELT入口。PLC、FEC、异常包及其他合法包继续走原始路径。

通过条件：真实包、随机有效包、截断包和PLC回退测试通过；PCM逐字节一致；Decode cycles或
P99达到保留门槛。

### T4：I-cache布局和cache coloring

1. 从ELF/map生成热函数地址、大小和`(addr >> 5) & 0x1ff` set范围。
2. 分离Encode、Decode、PVQ/FFT和高频USB/BT ISR的冲突区。
3. 将错误处理、CTL和fallback拆入`.text.unlikely`。
4. 用显式section和链接顺序控制热簇；禁止继续猜测式整体搬RAM。
5. cached OCRAM只保留连续调用且A/B有效的PVQ/FFT/MDCT簇。

### T5：E907位精确热点内核

优先级：PVQ真实高频shape、forward FFT/MDCT、entropy小除法/归一化、CELT Q15/Q16向量运算。
Encode和Decode分开修改、分开验证；每个内核必须通过host differential和target checksum。

### T6：编译器与构建

1. 应用热点TU逐个比较`-Os/-O2`。
2. Opus热点文件单独比较`-O2/-O3`，禁止全库O3/Ofast。
3. 逐TU恢复builtin识别和jump table并检查反汇编。
4. 尝试新版Xuantie GCC，只重编Opus/app并与现有SDK/BT库链接。
5. LLVM或自建GCC只在ABI、bit-exact和真机结果均通过后采用。

### T7：SDK、USB、Bluetooth与CRC

- 测量USB FIFO/VDMA重装和ISR间隔，独立A/B Audio IN/OUT FIFO映射。
- 测量BT net_buf allocation、L2CAP enqueue到completion和pool high-water。
- BMX CPU/SDU/BLEM QoS一次只改一位，任意USB/BT回归立即否决。
- 以16-entry nibble CRC替代bitwise实现前，必须通过全部报告向量和真机功能；不恢复未经证明的
  256-entry候选。

### T8：384/400 MHz独立实验

仅在T0-T7后进行。384 MHz使用BCLK divider寄存器4得到约76.8 MHz；400 MHz使用divider 4
保持80 MHz。不得照搬SDK中384 MHz、divider 3的注释路径，因为它会使总线约96 MHz。

超频不默认晋升生产固件，必须经过BOD、反复冷启动、数小时BT/USB/Flash满载、高温、供电变化、
Opus checksum和掉电恢复测试。不在缺少电压规格时盲目提高VDDCORE。

## 7. 候选保留与回退规则

候选只有同时满足以下条件才保留：

1. 两轮90秒正式负载加权cycles改善至少1%，或P99改善至少0.25 ms。
2. P95/P99/max、combined-window、queue age及任一硬错误不得稳定恶化。
3. 协议向量、host differential、USB枚举、speaker/mic/haptics功能通过。
4. 码率、采样率、声道、带宽、帧长、complexity和测试默认值未变化。
5. 保存ELF/bin/map/SHA256、构建参数和完整日志。

失败候选保存日志和结论后撤销源码，不产生“性能最优”提交。低于门槛的噪声结果不累计合并到
下一候选。

## 8. Git与性能账本规则

- spec、诊断基础设施、每个有效优化分别独立提交。
- 每个有效提交更新`docs/M61_PERFORMANCE_BASELINES.md`。
- 最优产物保存到`artifacts/m61-best/<suite>/current/`，历史候选保存到
  `artifacts/m61-history/<candidate>/`。
- manifest记录项目commit、SDK commit、工具链、CFLAGS、ELF SHA256、section大小和热函数地址。
- 只stage本阶段明确修改的文件；不得顺带提交用户现有未关联改动或PDF导出物。

## 9. 当前执行顺序

| 顺序 | 阶段 | 状态 |
| ---: | --- | --- |
| 1 | T0轻量计时、combined-window、outlier | 进行中 |
| 2 | T1 absolute-deadline执行器 | 待开始 |
| 3 | T2固定encoder入口 | 待开始 |
| 4 | T3 D4 decoder深层入口 | 待开始 |
| 5 | T4 I-cache布局 | 待开始 |
| 6 | T5 E907热点内核 | 待开始 |
| 7 | T6编译器 | 待开始 |
| 8 | T7 SDK/USB/BT/CRC | 待开始 |
| 9 | T8超频 | 待开始 |
