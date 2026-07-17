# 性能与基准规则

[English](PERFORMANCE.md)

## 目标

优化目标是DualSense输入、输出、扬声器/耳机音频、HD haptics和麦克风同时工作时连续。
尾延迟优先于小幅平均值改善：

1. queue、deadline、stale、Bluetooth、codec、packet-shortfall错误全部为0；
2. 降低encode/decode P95、P99、max；
3. 降低Bluetooth build/send max和audio pair age；
4. 最后才优化平均cycles。

质量不可改变：48 kHz、16-bit主机音频、声道布局、160 kbit/s speaker Opus、medium-band、
10 ms codec帧和手柄包格式都不能为了跑分降低。

## Release性能配置

构建锁把实测选项设为默认：USB/codec编译单元局部`-O2`、Opus fixed-point
`-O2 -flto`、E907位精确指令、精确512:480重采样、D4 decode parser快路、
decode-MDCT SRAM放置、Flash 16项CRC表和1 ms codec成对窗口。全部profiling关闭。

release内存结果：

| 区域 | 已用 | 容量 |
| --- | ---: | ---: |
| Application RAM | 215,584 B | 319 KiB |
| Non-cache alias使用 | 10,888 B | 319 KiB alias |
| WRAM应用section | 16 B | controller预留后128 KiB |
| ROM/XIP | 873,432 B | 4 MiB |

## 固定真机负载

`full-duplex-v1`运行90秒，包含四声道48 kHz/16-bit USB OUT、speaker、HD haptics、
20 ms HID output活动，并打开USB mic endpoint接收手柄实时Opus。`speaker-only-v1`
保持同样OUT/HID负载但关闭mic decode，不能与full-duplex排名。

每轮必须报告：

- encode/decode average、P50、P95、P99、max；
- 平均cycles（诊断构建还记录instret/cache）；
- mic queue age和USB IN underflow增量；
- epoch/drop/deadline/stale计数；
- Bluetooth alloc/send/retry/error计数；
- 主机packet shortfall和主观音频/震动表现。

## 当前晋升结果

权威表为[`benchmarks/PERFORMANCE_BEST.csv`](../benchmarks/PERFORMANCE_BEST.csv)。
当前Bluetooth/尾延迟默认是`4f8dfea`、`992111b`之上的`6bf8714`。三轮400 MHz
full-duplex结果：

| 指标 | 第1轮 | 第2轮 | 第3轮 |
| --- | ---: | ---: | ---: |
| Encode avg/P95/P99/max ms | 3.673/4.500/4.750/5.138 | 3.483/4.250/4.750/5.138 | 3.530/4.250/4.750/5.138 |
| Decode avg/P95/P99/max ms | 2.735/4.000/4.000/4.530 | 2.762/3.750/4.000/4.687 | 2.762/3.750/4.000/4.795 |
| Codec平均cycles | 2,566,094 | 2,500,302 | 2,518,108 |
| Mic underflow增量 | 11（启动） | 0 | 0 |
| 全部硬错误 | 0 | 0 | 0 |

同一CRC优化把profiled Bluetooth total平均从5.450降到4.113 ms，max从36.297降到
18.962 ms。

320 MHz stereo/mic-off只代表speaker容量：encode avg/P95/P99/max为
6.026/7.000/7.500/7.655 ms，硬错误为0。

## 晋升规则

优化必须通过位精确host测试、离线门禁、锁定release构建、重复90秒真机测试、全部硬错误
为0以及用户可见功能检查后才能晋升。仅诊断stage变快不够；多个SRAM放置和展开实验因
release P95/P99回归而被否决。

有效修改立即提交并加入CSV。否决方案直接回退，不为了“刷回旧版”单独发布固件。
