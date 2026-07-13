# M61 DualSense 固件性能与长期维护重构规范

状态：执行中  
目标平台：Ai-M61-32S-Kit / BL616、BL618 SDK 兼容目标  
基准工具链：Bouffalo SDK T-Head RISC-V GCC，通过 WSL 构建

## 0. 实施记录

- 2026-07-12：完成阶段 A。
  - epoch ingress 从逐 PCM 帧加锁改为每 chunk 两次短临界区。
  - 1024 帧 host 回归测试的 lock 次数固定为 4 次。
  - slot 增加 READING/CANCELLED_READING 所有权，覆盖 encode/read 期间 reset。
  - 大 slot 清零、Opus 复制和 pair 复制移出全局关中断区域。
- 2026-07-12：阶段 B 已实现并进入真机验证。
  - USB OUT VDMA 直接写入 8 槽、32 字节对齐的 non-cache ring。
  - ISR 不再复制 392 字节音频包；slot 使用 DMA_ACTIVE/READY/READING 状态。
  - ingress task 改为 ISR task notification 唤醒，移除 1 ms 轮询。
  - WSL 目标构建成功；RAM 使用：non-cache 4744 B，cache RAM 180048 B。
  - 已刷入并通过 SHA 校验，等待人工 reset 后执行 USB/蓝牙/音频回归。
- 2026-07-12：首次阶段 B 真机测试发现任务优先级倒置。
  - ingress 无 gap/drop，ring high-water 为 2，排除 VDMA ring 吞吐不足。
  - Opus encode 平均约 9.6 ms；codec backlog 连续运行时饿死低优先级 BT bridge。
  - 观测到 epoch drop 8761、realtime stale 88，直接表现为音频和震动卡顿。
  - 调整为 bridge > ingress > codec > auto/LED，重新进入刷写验证。

## 1. 目标

本规范定义固件的长期终态，不以最小可行实现为目标。所有改动必须同时满足：

1. 优先降低实时链路的 P95/P99 延迟、最大关中断时间、丢包和音频欠载。
2. 数据所有权、ISR/任务边界和缓冲生命周期必须明确，可由单元测试验证。
3. release、profile 两种固件必须可复现构建，禁止复用导致配置漂移的 CMake cache。
4. 不以理论上的 ITCM、PSRAM 或更高优化等级替代实测；任何微架构优化必须 A/B 验证。
5. 每个阶段结束时保持离线测试通过、WSL 固件可编译，并保留性能回归指标。

## 2. 已核实的硬件约束

依据 `bl616_bl618_ds_zh_cn_2.5_open_.pdf` 与
`bl616_bl618_rm_zh_cn_0.98_open_.pdf`：

- E907 为 32 位、单发射、顺序执行 CPU，最高 320 MHz。
- I-Cache 为 32 KB，D-Cache 为 16 KB，均为两路组相连。
- CPU/Cache 可运行于 320 MHz，系统/外设总线典型上限为 80 MHz。
- OCRAM/WRAM 的 `0x62...` 地址经过 Cache/AXI；`0x22...` 为非缓存 AHB 别名。
- USB Device 内置专用 VDMA，具有共享的 512 字节 FIFO，并支持多缓冲配置。
- 通用 DMA 外设列表不包含 USB；USB 数据搬运应使用 USB VDMA。
- 产品规格中的 SRAM 容量与芯片手册存在口径差异，容量判断以链接脚本和最终 map 为准。

## 3. ITCM 策略

项目不得默认将应用热点迁移到 `.tcm_code`。

当前 SDK 链接脚本把名为 `itcm` 的 section 放在 `0x62FC...` 缓存 OCRAM 地址，
它不是手册中可独立确认的专用零等待存储器。真机已经观察到 ITCM 版本性能下降，
因此默认策略为：

- 应用代码保留在 Flash XIP，通过 I-Cache 执行。
- 仅 Flash/Cache 切换等不能从 XIP 执行的底层代码保留在 SDK 的 TCM section。
- 只有单函数 A/B 测试同时改善 cycles、P99 和 cache miss 时，才允许例外迁移。

## 4. 终态实时数据流

```text
USB VDMA non-cache ring
        │ ISR：提交索引、重装 VDMA、通知任务
        ▼
audio ingress owner task
        │ 锁外填充 epoch，短临界区发布状态
        ▼
audio codec task
        │ Opus 编解码，完成后短临界区发布
        ▼
BT TX scheduler
        │ 使用 slot handle/版本，不复制大型 union
        ▼
BR/EDR L2CAP TX
```

反向麦克风链路使用固定 192 字节 USB 包队列：codec task 为唯一生产者，
USB IN ISR 为唯一消费者。禁止在全局关中断期间逐字节操作环形缓冲区。

## 5. 并发与临界区规则

1. USB endpoint callback 按 ISR 上下文设计，不得在其中执行大块 `memcpy/memset`、
   PCM 转换、日志格式化或循环扫描。
2. 单生产者/单消费者队列使用 head/tail 发布协议；数据写完后才能发布 head。
3. epoch slot 必须有明确所有权状态，例如 FREE、FILLING、READY_ENCODE、
   ENCODING、COMPLETE、READING、CANCELLED_ENCODING。
4. 大块清零、PCM/haptics 运算、Opus 数据复制必须在临界区外完成。
5. 临界区只允许进行状态、索引、版本号和少量统计字段更新。
6. 所有关中断入口统一计时；性能报告不得只覆盖 USB lock 而遗漏 epoch lock。

## 6. 调度规则

- 数据到达使用 FreeRTOS task notification/event 唤醒，不使用 1 ms 永久在线轮询。
- 周期性状态刷新使用带超时的阻塞等待。
- ingress、codec、bridge 的优先级必须通过最坏情况 backlog 测试确定，禁止依赖
  `taskYIELD()` 让低优先级任务获得运行机会。
- BL616 SDK 默认蓝牙线程优先级为 HCI TX `configMAX_PRIORITIES - 3`、BT RX 与
  Controller RX `configMAX_PRIORITIES - 4`。本项目不再覆盖蓝牙栈优先级；codec
  与 BT RX/Controller RX 同为 `configMAX_PRIORITIES - 4`，bridge 为
  `configMAX_PRIORITIES - 5`，ingress 与 HCI TX 同为 `configMAX_PRIORITIES - 3`。
  codec 每轮最多编码一帧并强制阻塞 1 tick，为 bridge 提供确定的发送窗口；禁止
  恢复连续多帧 catch-up。
  修改 SDK、FreeRTOS 最大优先级或配置后必须重新核实实际宏值，并由编译期断言
  阻止 bridge 高于 BT RX。
- 内存不足和 `-EAGAIN` 重试采用事件或有界退避，不得每毫秒无条件重试。

2026-07-12 真机回归曾将 bridge 提升到 `configMAX_PRIORITIES - 3`，随后出现持续
`Unable to find conn for handle 128, hci_acl`、Steam 仅保留枚举但输入停止、DSX 无法
正常识别。该优先级布局判定为无效，禁止再次使用。

同日第二次回归把 codec 从历史可用的优先级 27 降到 24。`opus_encode()` 的墙钟
平均耗时从约 9.6 ms 增至 14.5 ms，导致约 45% speaker epoch 触发 deadline fallback。
这不是 Opus 算法或 VDMA 变慢，而是测量区间包含了高优先级任务抢占。后续性能报告
必须同时给出 HPM cycles/instret 与 wall-time，禁止仅凭 wall-time 判断热点本体退化。

恢复 codec 27、bridge 26 并禁止连续 catch-up 后，编码平均恢复到 9.393 ms，
但 max 仍为 12.107 ms；speaker cancel 约 10%、epoch qdrop 约 19%，BT realtime
stale=58。结论是 160 kbps Opus 已占用约 94% 的 10 ms CPU 周期，系统没有足够余量
完成 ingress、HID、组包和 BT TX。下一轮保持 200 字节协议包不变，仅将有效码率降至
64 kbps并使用标准 Opus padding，目标是释放真实 CPU 预算，而非继续移动任务饥饿。

64 kbps 真机验收结果：Opus last/average/max=5884/7457/10039 us；speaker
frames=19013、encoded=19014、cancel=0、qdrop=1；BT realtime stale/retry/drop=0；
USB ingress drop/gap=0。扬声器与震动体感恢复正常。本轮根因最终判定为 160 kbps
编码占满 CPU 周期，加上错误的连续 catch-up/优先级调整后，在 speaker 与 haptics
之间反复转移任务饥饿；VDMA ring 本身不是回归根因。

2026-07-12 后续长时间听音发现64 kbps会产生周期性的水下/桶盖式音色变化。协议中
每个10 ms Opus块固定预留200字节，对应160 kbps；64 kbps编码后再padding到200字节
虽然协议合法且显著降低CPU负载，但属于以编码质量换性能，不满足项目约束，永久撤销。
默认恢复160 kbps，并要求在该质量基线上继续优化Opus实现和底层热点。

### 2026-07-12：源码Opus编译优化A/B

- SDK预编译Opus 1.2.1在160 kbps下：平均8.169 ms，P95/P99=10.25/10.75 ms，
  max 11.402 ms；21,713帧出现qdrop 1,131、deadline/cancel各141、BT stale 8。
- 同版本上游固定点源码使用Xuantie GCC和E907 packed-DSP ISA重新以`-O2`构建后：
  平均6.845 ms，P95/P99=8.75/9.00 ms，max 9.417 ms；3,854帧中qdrop、
  deadline、cancel和BT stale均为0。平均改善约16.2%，且保持160 kbps/200字节。
- 全库`-O3`导致ROM和I-cache工作集膨胀：平均7.625 ms，P95/P99=10.00/10.25 ms，
  max 10.232 ms，I-cache miss rate从`-O2`的1.352%升到1.496%，并重新出现qdrop。
  因此全库`-O3`永久否决；默认使用源码`-O2`，后续只允许对独立热点文件A/B `-O3`。
- `build_opus.sh`固定下载Mozilla归档Opus 1.2.1并校验SHA256，构建产物位于项目
  `.cache/third_party`。保留`--opus-sdk`、`--opus-source-o2`和
  `--opus-source-o3`仅用于回归A/B，不修改共享Bouffalo SDK。
- 源码`-O2`进一步启用跨文件LTO后，平均6.240 ms，P95/P99=7.75/8.00 ms，
  max 8.878 ms；qdrop/deadline/cancel/BT stale继续为0。相对普通`-O2`再改善约
  8.8%，相对SDK预编译库累计改善约23.6%。因此默认改为`source-o2-lto`；普通
  `source-o2`只作为回归基线保留。
- E907 `SMALDA`双16位乘加实验覆盖`celt_inner_prod`和`dual_inner_prod`，最终在
  `run_prefilter`、`compute_theta`和`quant_all_bands`生成31处指令，但平均编码反而
  从6.240 ms升到6.568 ms，P95/P99从7.75/8.00 ms升到8.00/8.75 ms，平均周期从
  1,998,110升到2,102,444，I-cache miss rate从1.341%升到1.401%，并出现一次
  deadline/cancel。`quant_all_bands`由约3.7 KB膨胀到6.3 KB；64位寄存器对、对齐
  分支和代码工作集开销大于双乘加收益，因此该通用内积替换永久否决，不进入默认构建。
- 以上SDK、源码O2、O3与O2+LTO历史A/B均在Windows保持Audio OUT打开但不主动播放
  内容的条件下完成；USB仍持续发送零PCM并执行Opus编码，因此这些数据定义为“静音流
  基线”，不能替代有内容的全功能压力结果。
- 新增`run_m61_full_load.py`固定负载：48 kHz四声道，speaker为440/733 Hz，左右HD
  haptics为160/223 Hz，每20 ms发送一次改变LED字段的HID SetState，测试前后自动抓取
  `ds5 status`；默认低噪声幅度为speaker 600、haptics 4000。`compare_m61_perf_status.py`
  从累计计数计算测试区间独立平均值。
- Opus complexity 0会永久禁用CELT pitch prefilter，但上游仍构造临时`pre[]`窗口并运行
  零增益comb filter。新增位精确快速路径，在`enabled=0`且历史gain为0时只维护必需历史
  状态；1200个连续10 ms测试帧覆盖静音、脉冲、高频交替、斜坡与随机噪声，原版和补丁
  输出242,400字节完全一致，SHA256均为
  `681a4b66369f631056a54bf8612f058922e0e713b78a566f5034c43c77e62060`。
- 快速路径必须保持`run_prefilter`为noinline独立函数。允许LTO内联时
  `celt_encode_with_ec`增大542 B并抵消指令收益；隔离后其恢复为14,282 B，
  `run_prefilter`为2,414 B。相同低噪声固定负载下，平均编码从6075.5降到6035.0 us，
  cycles从1,949,370降到1,931,988，I-cache miss rate从1.408%降到1.388%；8438帧中
  qdrop/deadline/cancel/BT stale均为0。该补丁进入默认源码Opus构建。
- Xuantie GCC在当前`-march`下将Opus的`__builtin_clz()`生成为`__clzsi2`函数调用，
  但BL616的E907 packed-DSP支持单指令`clz32`。通过`ecintrin.h`为`EC_CLZ`提供E907
  内联实现后，最终固件生成217条`clz32`；Opus路径不再调用`__clzsi2`，text减少512 B，
  `celt_rcp`从192 B降到164 B。
- 相同低噪声固定负载下，`clz32`版本平均编码从6035降到5448 us，cycles从
  1,931,988降到1,745,161，instructions从283,316降到268,096，I-cache access/miss
  从278,902/3,871降到260,935/3,442；8438帧中qdrop/deadline/cancel/BT stale均为0。
  平均编码与周期均改善约9.7%，因此`M61_OPUS_E907_CLZ32`进入所有M61源码Opus变体。

## 13. 全双工与双声道性能目标

当前单声道 speaker 在恢复160 kbps后必须重新建立性能基线，不能直接宣称具有全双工
或双声道余量。麦克风与双声道启用前必须完成：

当前阶段 `CONFIG_M61_DS5_MIC_DEFAULT_ENABLED=0`：不得请求手柄麦克风流，不运行
Opus decode，不把mic负载混入基础线路优化。先完成输入、输出报告、震动和单声道
speaker的满载上限优化，再单独启用mic做增量预算验收。

1. 将 bridge 从 1 ms 轮询改为 task notification/event 驱动，并量化空闲唤醒次数。
2. 将 BT scheduler selection 改为 slot handle，删除 584 字节 selection 和大型 union
   的反复复制/清零；epoch pair 同样改用只读 handle 生命周期。
3. 将麦克风 USB IN 改为固定 192 字节 packet ring；禁止在全局关中断区逐字节搬运
   `mic_pcm_ring`，记录 decode last/P50/P95/P99/max。
4. 删除 `AUDIO_MIC_PAUSE_AFTER_SPEAKER_MS` 这种互斥式规避前，先建立 speaker encode、
   mic decode、bridge、BT RX/TX 的逐任务 CPU budget；全双工压力测试不得出现 underflow、
   stale、deadline fallback 或输入报告停顿。
5. 审计 SDK 预编译 Opus 1.2.1 `e907fp/libopus.a` 的 ISA、定点/DSP、编译优化与 cache
   行为；必要时建立可复现的源码构建版本并与预编译库 A/B。不得默认迁移到 ITCM。
6. 双声道必须以真实双声道 encoder state/CPU 测试为准；不得用单声道复制伪装通过。
7. 最低验收余量：speaker-only P99 不超过帧周期的 70%，full-duplex 总 codec P99 加
   bridge/BT/USB 最坏预算后仍保留至少 20% CPU；否则继续优化，不开放功能。

### 2026-07-12：scheduler selection handle化结果

- `m61_bt_tx_selection_t` 从 584 字节降至 32 字节，selection 不再清零/复制大型union，
  改为借用scheduler slot中的只读payload。
- 64 kbps speaker平均编码墙钟从7.457 ms降至7.040 ms（约5.6%），max从10.039 ms
  降至9.680 ms；qdrop从1降至0，cancel与BT stale保持0。
- bridge stack HWM从1091增至1235 words，约减少576字节峰值栈占用。
- 结论：改动具有可测满载收益但不是体感级跃升，应保留；下一目标是删除epoch pair到
  scheduler realtime slot之间的约528字节双重复制。

epoch直接写入scheduler空闲realtime slot后，平均编码墙钟由7.040 ms降至6.808 ms
（约3.3%），max由9.680 ms降至9.525 ms；qdrop/cancel/BT stale继续为0。相对最初
64 kbps稳定版7.457 ms累计改善约8.7%。bridge stack HWM未继续变化，说明大型局部
selection已是主要栈峰值。后续停止继续优化小型复制，转向Opus本体热点。

扩大样本后的最终真机值为平均6.550 ms、max 9.317 ms，qdrop/cancel/BT stale仍为0；
相对最初64 kbps稳定版累计改善约12.2%。epoch/scheduler复制优化验收通过，但边际收益
已经明显下降，基础数据搬运阶段结束。

## 7. 内存与 Cache 规则

- USB VDMA 缓冲区使用非缓存地址并按 32 字节对齐。
- epoch PCM、Opus state、BT scheduler 等 CPU 热数据保留在缓存 OCRAM。
- 不把实时热数据迁入 PSRAM；PSRAM 仅可承载非实时、低频访问数据。
- 队列元数据与大数据区分离并按 cache line 对齐，减少无关 cache line 访问。
- codec stack、Opus state 上限必须以真机 HWM/`opus_*_get_size()` 收缩，保留明确余量。

## 8. 构建规范

- 所有 M61 固件构建必须通过 WSL 执行。
- release/profile 使用独立构建目录或每次彻底重新配置，禁止配置残留。
- release 默认关闭 HPM、shell、coredump、USB INFO 日志和非必要蓝牙调试日志。
- profile 固件启用 HPM 和扩展实时指标，但不得作为 release 产物发布。
- profile 配置宏只能加到应用目标，禁止使用 SDK 全局 compile definition。2026-07-12
  曾因 `CONFIG_M61_HPM_PROFILE` 传播到 RF/PHY/BT/驱动目标导致真机启动报
  `PHY RF init failed`；改为 `target_compile_definitions(app PRIVATE ...)` 后恢复。
- LTO 只有在编译和链接阶段同时启用时才算有效。
- 全局保持保守优化；热点文件优先比较 `-Os` 与 `-O2`，不默认使用 `-O3`。

## 9. 性能验收指标

每个阶段至少记录：

- Opus encode cycles/us：P50、P95、P99、max。
- USB ingress age：P50、P95、P99、max。
- 全局最大关中断 cycles，以及各临界区分类最大值。
- I-Cache、D-Cache read miss rate。
- audio ingress drop/gap、epoch drop/discontinuity、mic underflow。
- BT realtime stale/retry/drop、L2CAP `-ENOMEM/-EAGAIN` 次数。
- 任务 stack high-water mark 和剩余 heap。

硬门槛：重构不得增加数据损坏、epoch 顺序错误、USB 枚举失败或长期稳定性回归。
若平均值改善但 P99、drop 或 underflow 恶化，则判定优化失败。

### 2026-07-12：首轮 HPM profile 基线

- 20,197 个持续 speaker encode 样本：last/average/max=
  6.426/6.592/9.558 ms，P50/P95/P99=6.50/8.25/8.75 ms。
- cycle last/average/max=2,055,798/2,110,105/3,058,161，平均 retired
  instructions=287,598。该区间包含 encode 期间的中断抢占，不能解释为纯 Opus CPI。
- I-cache access/miss=291,140/4,206，miss rate=1.4447%；D-cache read/miss=
  59,981/1,123，miss rate=1.8730%。
- USB ingress age last/P95/P99/max=36/500/750/885 us；最大关中断=6,607 cycles。
- speaker qdrop/odrop/cancel=0，haptics qdrop/deadline=0，BT realtime
  stale/retry/drop=0，USB ingress drop/gap=0；mic仍关闭且没有执行Opus decode。
- profile 平均值相对同版本 release 的6.550 ms约增加0.6%，采样开销可接受；但
  P99仍占10 ms帧周期的87.5%，未达到7 ms硬目标。下一阶段停止优化小型复制，集中
  审计Opus/CELT热点、预编译库优化参数、DSP内核覆盖和cache布局。

### 2026-07-13：E907 32x16 Q15 乘法候选

- CSI DSP整套CFFT只支持16到4096的2次幂长度，不能直接替换CELT 10 ms路径使用的
  混合基数FFT；直接替换会改变长度、缩放和twiddle语义，因此否决。
- Xuantie GCC把`MULT16_32_Q15`展开为多个`mul`、移位和加法；E907的`smmwb`与
  Opus官方ARMv5E路径采用的高半字乘法等价，可用`smmwb+slli`完成。
- 单独编译的静态结果：`kiss_fft` text 3272降至2252 B，普通`mul` 119降至3，
  新增`smmwb` 59；`mdct` text 1860降至1272 B，普通`mul` 68降至4，新增
  `smmwb` 33。该方向同时降低执行指令与I-cache占用。
- 数值语义相对通用精确`>>15`最多相差1 LSB，与Opus已有ARMv5E固定点实现一致。
  1200帧连续编码均可正常解码；两版本解码PCM之差为56.106 dB SNR，最大绝对差404。
  这不是降低采样率、码率、声道或频带，但仍必须通过M61真机音质和HPM指标后才能保留。
- 真机固定全负载两轮结果中，`instret`稳定从268096降至约251370（-6.24%），但
  第一轮cycles/encode为1746168/5451 us，第二轮为1755124/5479 us；相对`clz32`
  基线1745161/5448 us分别持平和恶化0.57%，I-cache miss也没有下降。E907上减少退休
  指令没有转化为执行时间，且该实现引入最低位数值差异，因此判定失败并从默认构建回退。

### 2026-07-13：E907硬件精确倒数候选

- `celt_rcp`在宿主profile约占9.4%，通用实现使用5次乘法和两轮Newton迭代生成
  最大相对误差约7.05e-5的Q16倒数。E907 M扩展可用一条`divu`直接计算
  `2^31/x`，数学误差更小，不降低码率、采样率、声道或频带。
- 独立目标对象中`celt_rcp`由164 B缩至26 B。1200帧连续编码均可正常解码；相对
  旧近似实现的解码PCM差异SNR为51.662 dB，最大绝对差433。编码决策会因更精确的
  倒数而变化，因此仍需真机cycles、P99、drop和主观音质共同决定，不能仅凭代码缩小保留。
- LTO最初把26 B实现复制进198个调用点，导致镜像和I-cache膨胀；为`celt_rcp`添加
  `noinline`后，最终ELF只保留一个26 B函数和一个新增`divu`，该约束是优化的一部分。
- 固定全负载真机第一轮：encode/cycles/instret=5385 us/1724226/256218；第二轮区间
  为5429 us/1738288/256912。两轮累计平均约5407 us/1731257 cycles/256565 instret，
  相对`clz32`基线5448 us/1745161/268096改善约0.75%/0.80%/4.30%。
- 累计P95/P99由6500/6750 us改善至6500/6500 us，最大编码时间由7359降至7119 us；
  speaker、haptics、BT、USB的drop/deadline/error均为0。宿主两版解码结果相对同一
  原始PCM的SNR差小于0.001 dB，未检测到质量下降。该方向通过门槛并进入默认构建。

### 2026-07-13：PVQ非负Q15平方局部内核（复现审计后否决）

- 原实验把`op_pvq_search_c`两处非负Q15平方替换为位精确`khmbb`，当时记录为
  5025 us/1609246 cycles/249576 instret，并误判为约7%收益。
- 从干净Git worktree重建后，最终ELF确实只有2条`khmbb`，`quant_partition`仅缩小
  4 B。严格baseline-v1 A/B中，PVQ版为5394 us/1727302 cycles/256616 instret，
  no-PVQ版为5370 us/1720199 cycles/255034 instret；PVQ反而慢0.45%，cycles高0.41%，
  instret高0.62%，双方drop/deadline/stale均为0。
- 旧5025 us结果无法由已提交源码重现，说明当时构建缓存中混入了未记录状态。该数字和
  “累计19.5%”结论作废；补丁、宏和默认构建入口全部删除。当前可复现满载基线采用
  no-PVQ的约5.37 ms，相对源码O2+LTO的6.240 ms改善约13.9%。
- 后续`sqrt khmbb`和精确`mul+mulh`实验在各自同一构建状态内仍相对其配对基线恶化，
  否决结论保留，但不得再把5025 us作为跨版本绝对基线。

### 2026-07-13：`celt_sqrt` Q15 Horner内核（否决）

- `celt_sqrt`的归一化输入`n`严格位于[-16384,32767]，因此4处Q15 Horner乘法也可
  位精确替换为`khmbb`。静态结果为函数168降至160 B、4条`khmbb`；并通过
  `noinline`避免LTO把函数复制到多个调用点。
- 真机第一轮为5079 us/1625452 cycles/247731 instret；第二轮为5257 us/
  1684830 cycles/252327 instret。累计5168 us/1655141 cycles/250029 instret，
  相对PVQ基线5025 us/1609246/249576，时间与cycles均恶化约2.85%，P99从6500恶化
  至6750 us。Horner链存在连续数据依赖，`khmbb`延迟无法被并行隐藏，因此否决并回退。

## 10. 分阶段实施

### 全功能冗余硬约束

- 优化目标是未来扬声器双声道、麦克风、HD haptics和输入同时运行，不把“功能暂时
  关闭”产生的空闲预算计为最终性能收益。
- mic当前保持对外不可用，但必须单独测量并优化实际48 kHz、mono、10 ms、71字节
  Opus decoder路径；禁止恢复speaker活跃后暂停mic decode 250 ms的旧退让逻辑。
- 优先优化encoder和decoder共用的Opus/CELT底层，以及USB→codec→BT数据流、任务
  抢占和cache；不采用“不操作就不发包”等无法覆盖全功能并发的小技巧作为上限方案。
- 算术优化必须位精确；非位精确候选只有在严格证明客观质量不下降并通过真机听感、
  P99、drop和deadline门槛后才能保留。

### 2026-07-13：71字节mic decoder独立基准

- `M61_DS5_MIC_OPUS_LEN`的实际协议值为71字节。新增固定流工具参数和纯decoder runner，
  使用48 kHz、mono、10 ms、56800 bps CBR生成12,000帧；全部packet严格为71字节，
  每帧稳定解码为480 samples。profile重复解码1,200,000帧，不混入编码时间。
- 当前fixed-point、禁SIMD宿主decoder平均约10.83 us/frame。热点为`opus_fft_impl`
  19.49%、`clt_mdct_backward_c`16.12%、`decode_pulses`9.67%、`deemphasis`8.83%、
  `quant_partition`6.60%。FFT和inverse MDCT合计35.61%，是encoder/decoder共用优化首选。
- decoder约为同环境encoder耗时的一半；这说明当前mono speaker约5 ms的真机预算叠加mic
  后很可能接近10 ms，尚未包含speaker stereo增量，必须继续降低共用变换和数据流成本。

### 2026-07-13：位精确`mul+mulh` 32x16 Q15（否决）

- 用`mul+mulh+srli+slli+or`重建完整48-bit乘积右移15位；边界组合和100万组随机
  输入均与`(int64_t)a*b>>15`位精确。1200帧encoder流逐字节相同，24,000帧71 B
  decoder PCM checksum相同。静态FFT 3272降至3008 B，MDCT 1860降至1660 B。
- 真机第一轮却为5779 us/1850990 cycles/258080 instret，P50/P95/P99=
  6000/6750/7000 us；相对PVQ基线5025 us/1609246 cycles/249576 instret，时间与
  cycles恶化约15%，I-cache miss从3137增至3758。E907 `mulh`高延迟超过所省ALU
  指令，单轮已越过硬门槛，因此停止第二轮并回退。

### 2026-07-13：真机decoder并发基准设施

- profile固件新增`ds5 decoder-bench on/off`。它不启用mic USB Audio IN，也不向
  Bluetooth发送mic-active；仅在每次真实speaker encode成功后，用真实48 kHz mono、
  10 ms、71 B CBR非静音Opus包解码一帧并丢弃PCM。
- decoder状态只由codec task在开关边界执行`OPUS_RESET_STATE`，避免shell task并发修改
  Opus状态。独立记录decode us/cycles/instret、I/D-cache和P50/P95/P99；benchmark帧和
  错误不混入真实mic统计。
- 非profile固件拒绝该命令，真实mic路径也不增加计时器读取开销。该设施用于判断现有
  `encode -> decode -> 1 tick让步 -> BT bridge`调度是否越过10 ms，并为后续是否拆分
  encoder/decoder task、bridge是否事件驱动提供真机证据。

### 2026-07-13：encode + synthetic decode真机并发基线

- 相同60秒四声道负载下，speaker encode平均5597 us/1792676 cycles，synthetic
  mic decode平均4111 us/1313852 cycles；两者纯codec平均已达9708 us。encode与decode
  的P99分别为6750和5000 us，尚未计入bridge与调度即可能越过10 ms。
- 60秒约6000个输入epoch只完成4905次encode，qdrop=1465、deadline=720，BT realtime
  stale=26。decoder加入后encode相对单独基线也上升约11%，I-cache争用不可忽略。
- 结论：调度重构能减少deadline和让BT及时发送，但无法单独提供双声道speaker+真实mic
  冗余；必须同时继续优化encoder/decoder共用FFT/MDCT与cache布局。

### 2026-07-13：mono deemphasis专用路径（否决）

- 把48 kHz mono、无downsample、无accum路径从通用deemphasis中分离，算式逐样本不变；
  24万帧和120万帧decoder checksum均完全一致，宿主耗时下降约1.1%。
- E907真机decoder仅从4111降至4105 us、cycles从1313852降至1309876（约0.30%），
  instret反从178329升至178946。收益低于噪声和硬门槛，且代码增加256 B，因此否决并
  从默认patch列表移除。
- synthetic decoder会让当前codec预算越过10 ms，只能作为显式压力测试。固定负载工具
  使用`--decoder-bench`临时启用并在正常退出或异常退出时自动关闭；日常功能测试必须
  保持`usb_decode_perf enabled=0`。

### 2026-07-13：FFT/MDCT固定表32字节对齐（否决）

- 将bitrev、twiddle和window固定表提升到32字节对齐；最终ELF确认目标表地址均满足
  对齐，1200帧encoder输出逐字节一致，24万帧decoder PCM checksum一致。
- no-PVQ严格基线为5370 us/1720199 cycles/255034 instret；对齐候选为5628 us/
  1802717 cycles，时间和cycles均恶化约4.8%，I-cache miss从3397升至3636。
- 固定表的额外padding和布局变化扩大了热指令/数据工作集，没有减少真机cache压力。
  候选已从源码移除；该结果再次证明对齐不能只凭静态直觉保留，必须以最终ELF和真机
  baseline-v1共同验收。

### 2026-07-13：E907位精确Q16乘法（通过）

- E907 `smmwb`返回signed word与signed low16乘积的bits 47:16，严格等价于
  `MULT16_32_Q16(a,b)`的`((int64_t)(int16_t)a*b)>>16`语义。与曾产生1 LSB差异的
  Q15候选不同，本候选不需要额外移位，也不改变任何编码决策或PCM结果。
- 边界组合和100万组随机输入与Opus非`OPUS_FAST_INT64`通用拆分公式完全一致；1200帧
  encoder流逐字节相同，SHA256均为`681a4b66369f631056a54bf8612f058922e0e713b78a566f5034c43c77e62060`；
  71 B mic流重复解码24万帧，两版checksum均为`08cbb480`。
- 最终LTO ELF确认生成5条`smmwb`，静态RAM门槛通过且未增加ITCM放置。两轮独立
  baseline-v1共17,163帧，平均5340 us/1708397 cycles/251818 instret；相对no-PVQ
  基线5370 us/1720199/255034分别改善0.56%/0.69%/1.26%。P99保持6500 us，speaker、
  haptics、BT和USB的drop/deadline/stale/error均为0。
- 该候选位精确且两轮时间、cycles和instret方向一致，因此进入默认O2+LTO构建。当前
  可复现累计平均约5.34 ms，相对源码O2+LTO的6.240 ms改善约14.4%。

### 2026-07-13：E907位精确Q15 `kmmwb2`（通过）

- E907 `kmmwb2`直接返回signed word与signed low16乘积的bits 46:15，等价于
  `MULT16_32_Q15`的算术右移15位。它只在`INT32_MIN * INT16_MIN`时饱和为
  `INT32_MAX`；该数学结果是`+2^31`，超出此Opus宏明确要求的32位结果有效域，因此
  对全部合法输入位精确。此前`smmwb+slli`候选会丢失product bit 15，不能替代本实现。
- 边界组合和100万组随机输入验证全部通过，唯一排除项正是上述越域组合。1200帧
  encoder流逐字节相同，SHA256均为`681a4b66369f631056a54bf8612f058922e0e713b78a566f5034c43c77e62060`；
  71 B mic流重复解码24万帧，两版checksum均为`08cbb480`。
- 最终LTO ELF包含161条`kmmwb2`；`opus_fft_impl`普通`mul`从117降至1，text从
  0xBAC降至0x73C。全固件text从859154降至855826 B，减少3328 B；静态RAM不变，
  ITCM布局不变。
- 三轮独立baseline-v1共25,315帧，分别为5229/5389/5243 us，累计平均5287 us/
  1693169 cycles/226993 instret。相对已提交Q16基线5340 us/1708397/251818，累计
  改善约1.00%/0.89%/9.86%；P99从6500改善至6250 us。三轮speaker、haptics、BT、
  USB的drop/deadline/stale/error均为0。
- 该候选进入默认O2+LTO构建。当前可复现平均约5.287 ms，相对no-PVQ的5.370 ms改善
  约1.55%，相对源码O2+LTO的6.240 ms累计改善约15.3%。

### 阶段 A：epoch 所有权

- 消除逐 PCM 帧关中断。
- 消除锁内 2344 字节 slot 清零、200 字节 Opus 复制和 pair 大复制。
- 扩展 host 单元测试覆盖 reset/encoding/read ownership 竞争。

### 阶段 B：USB VDMA/SPSC

- OUT endpoint 直接写入非缓存 ring slot。
- ISR 仅发布 slot、重装 VDMA、通知 ingress。
- 移除 ISR→缓存队列→任务局部变量的双重复制。

### 阶段 C：麦克风和事件驱动

- 使用 192 字节 packet ring。
- ingress、codec、bridge 改为 notification/event 驱动。
- 建立 backlog 和优先级压力测试。

### 阶段 D：BT scheduler 与报告生成

- selection 改为 slot handle，删除大型 union 清零/复制。
- 减少 epoch pair 的中间副本。
- A/B 测试 nibble-table CRC 和热点文件 `-O2`。

### 阶段 E：release/profile 和真机回归

- 固化独立构建产物和 manifest。
- WSL 全量编译并检查 map、stack usage、配置宏。
- 刷写后必须人工按开发板 reset；也可以让电脑 USB 端口重新上电。
- reset 前不把“刷写成功”视为固件已经运行。
- 完成至少 5 分钟全功能压力测试和长时间稳定性测试。

## 11. 刷写操作约束

刷写工具进入下载模式并写入完成后，开发板不会自动重启。标准流程为：

1. 通过 WSL 编译并生成固件。
2. 执行刷写并确认写入校验成功。
3. 提示操作者手动按 reset；或对电脑 USB 端口执行断电再上电。
4. 重新捕获启动日志，确认运行的是本次构建版本。
5. 再开始 USB 枚举、蓝牙连接和性能采样。

## 14. DualSense 音频、震动与连接交互基线

外部协议实现只作为交叉验证，不直接照搬：

- SAxense: https://github.com/egormanga/SAxense
- DS5Dongle: https://github.com/awalol/DS5Dongle
- DualSense结构资料:
  https://controllers.fandom.com/wiki/Sony_DualSense#USB_2
- Linux `hid-playstation.c` 作为传统rumble标志位的额外独立参考。

### 音频与HD haptics实线逻辑

- USB Audio OUT固定为48 kHz、4声道、16-bit：声道0/1是speaker，声道2/3是左右
  HD haptics。
- speaker每512个USB frame重采样为480 frame，再以48 kHz、10 ms Opus编码；
  24 kHz/240-frame实验会让DualSense无线音频持续输出巨大噪音，判定协议不兼容且
  违反音质不降级约束，永久否决。
- HD haptics无线负载是3 kHz、双声道、有符号8-bit；每64字节包含32个采样对，
  report 0x39一次承载连续两个64字节块。tag `0xD2`、长度64、packet counter加2
  与SAxense/DS5Dongle实测路径一致。
- 旧实现对每16个48 kHz样本做box average。该低通会让正负波形抵消，降低峰值，
  是无线震动力度弱于原生USB的高概率根因。改为相位连续的每16帧抽取一个样本，
  保持1.0增益和完整s8范围；该改变同时删除逐样本求和与除法，不牺牲speaker音质。
- 传统rumble必须原样转发 `HAPTICS_SELECT` 与新版
  `COMPATIBLE_VIBRATION2/EnableImprovedRumbleEmulation`。新版模式不应把力度减半；
  当前桥接不对motor值做缩放。

### 连接键与低功耗连接状态机

- Ai-M61-32S-Kit资料和BL616数据手册确认烧录键连接bootstrap GPIO2：高电平进入
  UART/USB下载，低电平从Flash启动。应用启动后将GPIO2配置为下拉输入，可读取运行时
  长按；若具体板卡不可读，`CONFIG_M61_PAIR_BUTTON_PIN`可改为预留外接GPIO0。
- 默认未连接状态只开启BR/EDR page scan，等待已配对DualSense按PS后主动连接；
  inquiry scan和discoverable关闭，不再循环主动连接保存地址或自动扫描。
- 长按连接键1.5秒启动一次有界inquiry。配对期间蓝灯闪烁；HID control和interrupt
  两条L2CAP通道均建立后蓝灯常亮；普通未连接状态RGB全灭。
- “connectable”和“discoverable”必须独立控制。作为DualSense主机，正常被动回连只需
  connectable；设备长期discoverable没有协议必要且增加射频活动。
- inquiry结果只能连接本轮实际发现的DualSense，禁止扫描失败后误用旧保存地址发起
  主动连接。incoming连接继续由手柄主动打开HIDP通道；本机inquiry发起的outgoing
  配对连接才主动打开HIDP通道。
- 主动创建的BR/EDR L2CAP通道必须在连接前显式设置`br.rx.mtu=672`。Bouffalo SDK将
  清零后的应用通道视为默认48字节MTU；如果不声明接收能力，BOOT发现模式建立的
  outgoing HIDP control/interrupt通道会协商为48，随后0x31/0x32和音频报告以
  `-EMSGSIZE`失败。incoming HIDP服务器原先能得到672，因此该故障只出现在主动发现
  连接路径。连接日志必须打印双向MTU，低于最大HIDP报告长度时明确报错。

### 板载状态灯

- GPIO12/14/15的RGB调光使用BL616 PWM0硬件完成，不使用高频软件PWM，不占用音频或
  蓝牙实时任务预算。按BL616 PWM引脚组映射，三脚分别是CH2P、CH3P、CH3N；亮度默认
  为12%占空比，可通过`CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE`调整。
- GPIO15使用CH3N负端，PWM0必须显式设为`PWM_IO_SEL_DIFF_END`；默认单端路由即使设置
  NEN也不会输出蓝灯。正负端极性均配置为active-high，仍可独立启停绿灯和蓝灯。
- 上电初始化阶段暗红灯常亮；蓝牙协议栈、HIDP服务器和被动page scan全部就绪后熄灭。
  普通未连接保持熄灭，长按连接键后的发现模式暗蓝闪烁，HID control和interrupt均
  建立后暗蓝常亮。灯状态切换只启停硬件PWM输出，不做周期性GPIO位翻转。
