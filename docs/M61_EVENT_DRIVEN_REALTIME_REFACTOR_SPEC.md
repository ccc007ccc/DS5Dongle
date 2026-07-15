# M61 事件驱动实时调度重构规范

## 1. 目标

在不改变 Opus 码率、带宽、采样率、声道语义、固定点结果和 DualSense 协议的前提下，
把当前分散的轮询与固定延时重构为“事件采集 + deadline 调度 + 受控执行”。主要目标不是
降低平均耗时，而是降低 encode/decode/BT 的 P95、P99、max 和排队年龄，并保持所有
stale、drop、qdrop、deadline、codec error 为 0。

重构保留 Bouffalo SDK、FreeRTOS、Bluetooth controller/host 和原生 USB。不得绕过 SDK
闭源 controller，也不得以降低音质或减少测试负载换取性能。

## 2. 核心原则

1. 事件表示“工作已就绪”，不表示“高优先级任务必须立即运行”。
2. 所有执行由中央实时调度器根据 absolute deadline、队列水位、预计成本和 BT 窗口决定。
3. 中断与协议回调只做时间戳、入队、ready 位更新和一次轻量唤醒，不运行 codec。
4. ready 使用位图或版本号，不使用无限累计通知计数，避免通知债务和空转。
5. 每次调度默认只运行一个重型阶段；encode 与 decode 连续执行必须由 deadline 证明必要。
6. BT realtime ready 时必须获得有界发送窗口，不能依赖低优先级任务碰巧被调度。
7. 保留一个低频 safety tick，用于 deadline、断连、漏事件和 watchdog；“全面事件驱动”不等于
   删除所有周期性安全检查。

## 3. 功能事件模型

| 功能 | 最终事件源 | 当前迁移适配器 | 是否允许立即执行重活 |
| --- | --- | --- | --- |
| speaker encode | 512-frame USB epoch ready | epoch ready 位 | 否 |
| mic decode | BT mic Opus 入队 | mic queue ready 位 | 否 |
| BT realtime | 相邻 epoch pair complete | realtime ready 位 | 仅唤醒 bridge |
| USB mic | PCM ring low-water/DMA complete | 现有 endpoint callback | 否 |
| HID按键输入 | BT HID input callback | 已是回调路径 | 否 |
| PC HID OUT/灯光/扳机 | USB OUT callback | 保持现有队列 | 否 |
| bridge普通状态/feature | USB/BT队列事件 | 1 ms轮询适配器 | 否 |
| LED/维护任务 | 状态变化或低频timer | 现有低优先级任务 | 否 |

按键位置必须长期保留为独立 `INPUT_READY` 事件类。当前正式性能测试继续使用1 ms bridge
轮询适配器，使测试环境、HID报告频率和负载不变；未来切换成纯事件唤醒时，只替换适配器，
不得改变调度器、按键映射或报告合并策略。

并非所有功能改成“零轮询”都会提高性能。高频且成本极低的轮询可能比频繁上下文切换更稳定。
最终准则是 P99/max 和全链路错误，而不是事件驱动覆盖率。

## 4. 调度状态

调度器维护以下 ready 位：

- `ENCODE_READY`
- `DECODE_READY`
- `BT_REALTIME_READY`
- `USB_MIC_LOW_WATER`
- `HID_INPUT_READY`（预留）
- `HID_OUTPUT_READY`（当前由轮询适配器设置）
- `CONTROL_READY`

每类重型job至少包含：`generation`、`created_us`、`deadline_us`、`queue_depth` 和
`estimated_cycles`。ready 位只可在对应队列为空后清除；generation变化必须原子取消旧job。

## 5. Deadline与预算

- speaker encode deadline：以 epoch capture time 为基准，首版使用 `+10,000 us`软deadline，
  BT配对发送使用独立更宽硬deadline。
- mic decode deadline：以BT包入队时间为基准，首版使用 `+10,000 us`。
- BT realtime硬年龄继续保持 `64,000 us`，不得为通过测试而放宽。
- 每10 ms预算窗口保留 `BT_MIN_WINDOW_US`，首版建议1,000 us；只有无realtime pending且
  两个codec job之一接近deadline时才可借用。
- 调度选择使用 earliest-deadline-first，但加入cache相位约束：刚完成encode时，若decode
  slack充足，先交还BT/USB；decode同理。
- 禁止无界catch-up。队列接近溢出时每轮最多追加一个重型job，之后必须重新评估时间。

## 6. Cache工作集隔离

1. encoder与decoder状态地址保持已验证地址，除非候选明确记录和复测布局变化。
2. 不把encode/decode交替频率当成越高越好；调度器记录上次重型阶段，优先减少无必要切换。
3. Opus TCM cluster和forward FFT优化保持不变。
4. 任何新增调度代码不得挤占现有27,208 B ITCM基线；纯策略代码默认留在XIP。
5. HPM诊断版与无HPM发布版分别验收，发布版不得因移除profile改变三个关键state地址。

## 7. 迁移阶段

### Phase A：策略模块与观测

- 新增纯策略调度器，输入ready/deadline/队列深度，输出一个action。
- 保持现有2 tick稳定执行器不变，以shadow mode记录“策略本会选择什么”。
- host test覆盖同时到达、deadline临近、BT pending、generation reset和漏事件。

### Phase B：受控codec执行

- ready位替换codec固定轮询的工作发现，但保留2 tick BT窗口作为上限保护。
- 每次只执行策略选择的一个codec阶段。
- 连续两轮全零后，才允许动态缩短窗口。

### Phase C：动态BT窗口

- 根据realtime pending、codec slack和队列水位在1～2 ms间调整。
- bridge保留1 ms普通HID轮询适配器，同时接受realtime直接唤醒。

### Phase D：输入与控制事件化

- 将按键/普通HID轮询替换为事件适配器，但保留编译开关恢复1 ms轮询负载。
- 比较相同报告频率下的CPU、P99和输入延迟；无收益则允许保留混合模式。

## 8. 验收与回退

每个执行阶段至少两轮90秒正式全双工，保持默认脚本和`--mic-input`：

- stale/retry/drop/reject/error = 0
- speaker/haptics qdrop/deadline/cancel = 0
- mic Opus/PCM drop和decode error = 0
- encode/decode P95、P99、max不得相对稳定基线恶化
- BT pair最大年龄必须下降或持平
- 实际听感不得出现盖革计数器式断续

任一硬错误出现即回退执行路径，但保留诊断证据。有效阶段独立提交，并更新
`docs/M61_PERFORMANCE_BASELINES.md`。稳定回退点为提交`d314597`及其2-tick调度固件。
