# 贡献指南

[English](CONTRIBUTING.md)

感谢改进M61 DualSense桥接。请先阅读[开发与验证](docs/DEVELOPMENT.zh-CN.md)和
[构建与刷写](docs/BUILDING.zh-CN.md)。

修改应保持聚焦，保留锁定release profile，为协议/算法变化添加确定性测试，并为依赖硬件
的行为提供真机证据。性能PR必须保持音频质量，在固定负载下报告average、P95、P99、max、
cycles、underflow和全部硬错误。

不要提交生成产物、SDK/工具链树、手柄私密数据、厂商PDF或包含个人设备信息的capture。
面向用户的文档修改必须同时更新英文和简体中文文件。
