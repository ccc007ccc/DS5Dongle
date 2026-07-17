# 开发与验证

[English](DEVELOPMENT.md)

## 仓库布局

```text
m61/dualsense_hidp_probe/   正式固件和构建系统
m61/usb_*_probe/            原生USB硬件bring-up探针
tools/                      host测试、刷写、采集和分析
benchmarks/                 已晋升的机器可读性能结果
docs/                       持续维护的中英文文档
```

构建输出、Opus源码/构建cache、日志、capture、二进制、厂商PDF和`artifacts/`都被忽略。

## 离线门禁

每个源码提交前运行：

```powershell
python tools\run_offline_checks.py
python tools\test_dualsense_crc32_nibble.py
python tools\test_m61_feature_bridge.py
python tools\verify_m61_build_environment.py --sdk C:\work\bl_mcu_sdk --toolchain-bin C:\work\toolchain_gcc_t-head_windows\bin
git diff --check
```

修改C/CMake、补丁、内存、工具链flag或构建脚本时必须运行锁定release构建。确认生成清单
为`\"profile\": \"release\"`，并对ELF执行RAM/TCM门禁。

## 真机门禁

功能修改要求：

1. 原生USB复合设备枚举；
2. Bluetooth control/interrupt通道连接；
3. 完整`0x31`输入活动；
4. output LED/震动/自适应扳机；
5. 相关的speaker、HD haptics、headset route和mic；
6. HID bridge附近修改必须验证Feature GET/SET页面；
7. 相关queue/codec/BT错误为0。

性能修改还必须重复固定90秒测试，只有晋升时才写入
`benchmarks/PERFORMANCE_BEST.csv`。

## 实时代码规则

- USB和Bluetooth回调必须有界、非阻塞。
- 用queue/state transition表达所有权，不用非正式共享flag。
- Bluetooth TX只能有一个策略点，codec统计只能有一个写者。
- generation/deadline元数据必须跨队列保留。
- release热路径不得随意分配、打印、读取HPM或切时钟；必须先证明必要性和成本。
- SRAM放置必须选择性进行；每次放置修改都要测release，不能只看stage profile。
- 保持Opus/PCM位精确以及全部质量不变量。

## 构建profile

Release设置只由JSON lock和构建脚本默认值定义。新增诊断必须有默认关闭的编译期开关并写入
manifest。不得把诊断固件当release baseline。

## 源码与文档策略

- 公开维护文档必须成对提供English和`.zh-CN.md`。
- 保留一份当前功能/架构/性能文档，不堆积按日期生成的计划日记；旧实验由Git历史保存。
- 不提交绝对路径、COM日志、配对数据、SDK树、编译器二进制或厂商PDF。
- 提交保持聚焦。有效优化和正确性修复独立提交；相应文档与性能表随修改更新。
- `master`是后续开发主线。高风险实验使用短期topic branch，只合并验证通过的结果。

## 管理协议开发

不要通过WebHID直接暴露shell文本。管理命令必须有版本化二进制schema、长度/范围校验、
capability discovery、有界异步执行、明确持久化语义和host测试；未知字段/版本安全失败。
