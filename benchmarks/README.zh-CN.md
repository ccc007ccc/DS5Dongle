# 性能数据

[English](README.md)

`PERFORMANCE_BEST.csv`是持续维护、机器可读的最优真机结果表。只有满足以下条件才新增：

- 固定负载和音频格式不变；
- 记录average、P95、P99、max、cycles、underflow增量和全部硬错误；
- 协议/PCM位精确测试通过；
- 新release默认至少通过两轮稳定性测试；
- 有效修改有独立Git提交。

Decode字段为空表示有意关闭麦克风负载，这种行不能与全双工结果比较。被否决实验保留在
Git历史中，不再堆积到当前最优表。
