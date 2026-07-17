# 依赖、许可证与再分发

[English](OPEN_SOURCE.md)

## 仓库边界

本仓库只保留M61应用源码、构建元数据、测试和持续维护文档，不内置1 GiB以上的Bouffalo
SDK、编译器二进制、生成固件、基准日志或厂商PDF手册。

SDK审计发现历史上只有链接脚本4行修改，CherryUSB、FreeRTOS和Bluetooth库源码没有本地
修改。该内存选择现在由项目级`CONFIG_WRAM_LENGTH=163840`表达，因此锁定的官方SDK
保持干净。单独维护SDK fork会增加超过1 GiB且已经不承载必要修改，所以不需要建立。

Opus不同：性能优化会修改codec源码。因此项目保存体积小、可审查的patch，按SHA256下载
精确upstream 1.2.1压缩包并在本地构建。这样既可复现，也保留upstream许可证声明，无需
把生成后的源码树塞进仓库。

## 许可证映射

| 材料 | 位置/来源 | 许可证 |
| --- | --- | --- |
| M61应用、工具、维护文档 | 本仓库 | MIT |
| 原始DS5Dongle工作 | `awalol/DS5Dongle`历史 | MIT |
| Bouffalo SDK | 外部锁定checkout | Apache-2.0及组件许可证 |
| CherryUSB | 通过Bouffalo SDK | Apache-2.0 |
| FreeRTOS | 通过Bouffalo SDK | MIT |
| Opus 1.2.1及M61补丁 | 下载源码加tracked patch | Opus BSD风格许可证/专利声明 |
| Sony名称/报告格式 | 互操作标识 | 不分发Sony代码、固件、密钥或美术资源 |

分发固件前阅读[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)。二进制分发者必须
附带适用的object-form声明，尤其是Opus版权和许可证全文。

## 厂商文档

芯片和板卡PDF是有用的开发参考，但可能有单独再分发条款。PDF副本放在被忽略的
`docs/vendor/`，不提交到Git。公开文档只记录构建和接线所需事实；用户应从厂商获取
最新手册。

## Release清单

应发布：

- 源码commit/tag；
- 需要时发布BIN、ELF、MAP；
- 生成的`.manifest.json`；
- MIT许可证和`THIRD_PARTY_NOTICES.md`；
- 说明精确硬件和profile的中英文release note。

不要发布：

- 复制进本仓库的SDK/工具链二进制；
- Opus build/cache目录；
- 私有手柄数据或配对密钥；
- 本地COM日志、绝对路径或未经许可的厂商PDF；
- 被标记为release的custom/unverified清单。
