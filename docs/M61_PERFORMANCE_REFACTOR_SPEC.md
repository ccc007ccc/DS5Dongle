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

## 13. 全双工与双声道性能目标

当前 64 kbps 单声道 speaker 平均仍消耗约 7.5 ms/10 ms，不能直接宣称具有全双工
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

## 10. 分阶段实施

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
