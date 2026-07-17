# 架构

[English](ARCHITECTURE.md)

## 系统边界

正式目标是单颗M61/BL616级SoC。FreeRTOS、Bouffalo蓝牙栈、CherryUSB和应用都保留在
同一个固件镜像中。

```text
                         M61 / BL616
DualSense  <---->  Bluetooth HIDP传输
                           |
                           v
                    报告/音频桥接
                           |
                           v
PC          <---->  CherryUSB复合设备
```

## 实时数据路径

### 手柄输入

1. 蓝牙HID interrupt回调接收`0x31`报告。
2. `dualsense_parser.c`校验并解析完整状态。
3. 最新状态被转换成有线USB输入布局。
4. endpoint `0x84`空闲时，USB input pump发送最新状态。

输入属于latest-state数据：过时的排队样本永远不比当前手柄状态更有价值。

### 手柄输出与Feature报告

1. CherryUSB接收主机output/Feature SET。
2. 应用复制到有界队列；USB回调不等待蓝牙分配。
3. 中央Bluetooth TX scheduler为实时音频、状态、Feature和诊断流量分级。
4. 蓝牙Feature响应保留Report ID并缓存，再返回原USB GET。对应`0x80` SET会使动态
   `0x81`页面立即失效。

### 扬声器与HD haptics

```text
USB OUT 48 kHz / 16-bit / 4声道 / 1 ms包
        |
        +--> ch0/ch1 --> 512帧epoch --> 精确512:480重采样
        |                                     --> Opus encode
        |
        +--> ch2/ch3 --> HD-haptics PCM
                               |
                               v
                 成对10 ms epoch --> BT 0x36
```

路由策略在手柄mono扬声器、stereo耳机或显式stereo/mono覆盖之间选择，不改变主机采样率
或位宽。

### 麦克风

```text
BT 0x36中的固定71-byte D4 Opus帧
        --> 有界Opus队列
        --> Opus decode（10 ms）
        --> mono PCM复制为stereo
        --> 10个1 ms USB Audio IN包
```

运行时可以停止mic处理而不改变USB枚举；Windows继续收到有意生成的静音。

## 调度与所有权

- SDK蓝牙任务负责手柄回调和协议推进。
- USB回调只进行有界reserve/copy/publish。
- codec任务是codec状态和计时的唯一写者。
- 音频epoch使用generation ID和明确所有权迁移。
- Bluetooth TX scheduler是实时/控制包准入的唯一策略点。
- DVFS请求交给worker；音频回调不直接重编程PLL。

SDK能提供可靠事件的地方采用事件驱动。半成品SDK缺少发送完成credit回调的地方仍保留
短服务窗口，但窗口是显式、可基准测试的。

## 内存与代码放置

release链接布局固定160 KiB WRAM，与实测最优固件一致，应用RAM区域为319 KiB。Opus
PVQ/MDCT热点集群和decode MDCT被选择性放入cacheable SRAM。禁止大范围把代码搬入RAM：
真机测试证明更大的代码工作集会增加I-cache冲突和尾延迟。

release内存门禁限制启动关键TCM与静态物理RAM；诊断buffer只在对应profile中编译。

## 主要源码模块

| 模块 | 职责 |
| --- | --- |
| `main.c` | 板级启动、Bluetooth HIDP、配对/重连、Feature桥接、shell |
| `m61_usb_gamepad.c` | USB描述符/endpoint、音频输入输出、Opus codec任务 |
| `m61_audio_epoch.c` | speaker/haptics epoch所有权及相邻pair组装 |
| `m61_bt_tx_scheduler.c` | 中央Bluetooth TX选择与准入 |
| `m61_realtime_scheduler.c` | deadline/readiness策略 |
| `m61_dvfs.c` | 运行时频率档位、governor、持久化和worker |
| `dualsense_parser.c` | 蓝牙/USB输入解析 |
| `dualsense_output.c` | 蓝牙output/Feature包及CRC |
| `m61_perf_profile.c` | 编译期开关控制的HPM聚合 |

## 配置边界

当前M61管理面是shell命令。稳定WebHID协议不会直接拼接原始shell字符串；Web重构将引入
版本化二进制capability/config协议，并由固件负责校验和持久化。
