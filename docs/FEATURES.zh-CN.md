# 功能与当前缺口

[English](FEATURES.md)

状态含义：**已验证**表示已有真机证据；**已实现**表示源码和离线测试覆盖但仍需更广泛
真机回归；**缺失**表示正式固件没有实现。

## 固件功能矩阵

| 范围 | 能力 | 状态 | 说明 |
| --- | --- | --- | --- |
| 蓝牙 | DualSense BR/EDR扫描与连接 | 已验证 | 名称过滤、地址直连、保存上次地址 |
| 蓝牙 | 配对/安全与bond重连 | 已验证 | SDK settings加固件last-address记录 |
| 蓝牙 | HID SDP及L2CAP PSM `0x11`/`0x13` | 已验证 | Control与Interrupt通道 |
| 输入 | 完整蓝牙`0x31`报告 | 已验证 | 摇杆、按键、扳机、触摸、IMU、电量、耳机位 |
| USB | Sony `054C:0CE6`复合枚举 | 已验证 | Audio Control、Audio OUT、Audio IN、HID |
| USB HID | 输入转发 | 已验证 | 完整DualSense形态USB报告 |
| USB HID | 输出转发 | 已验证 | LED、震动、自适应扳机和状态字段 |
| USB HID | Feature GET/SET代理 | 已验证 | 静态和动态手柄页面保留Report ID |
| Audio OUT | 四通道48 kHz、16-bit USB流 | 已验证 | 扬声器L/R加HD-haptics通道 |
| 扬声器 | 单声道、双声道、自动耳机路由 | 已验证 | Auto读取手柄耳机状态；左右映射已修正 |
| HD haptics | USB音频到蓝牙`0x36` | 已验证 | 与扬声器合包且不降低质量 |
| 麦克风 | 手柄Opus到USB Audio IN | 已验证 | 48 kHz、16-bit，手柄mono复制为两个USB声道 |
| 运行时音频 | 麦克风开关 | 已验证 | shell/API立即切换；默认关闭 |
| 运行时音频 | 扬声器路由切换 | 已验证 | `auto`、`mono`、`stereo` |
| DVFS | 固定档位和自定义320–400 MHz | 已验证 | Eco 320、balanced 384、performance 400 |
| DVFS | 实验401–480 MHz | 已实现 | 必须显式允许；不同板子稳定性不同 |
| DVFS | Realtime governor、floor和定时boost | 已实现 | 事件驱动worker，热路径不直接改时钟 |
| DVFS | 常驻频率策略保存/清除 | 已验证 | EasyFlash记录；实验频率不能保存 |
| 诊断 | `ds5 status`完整计数 | 已验证 | 队列、codec、USB、BT、Feature代理、haptics |
| 诊断 | 编译期开关HPM/pipeline/runtime profile | 已验证 | release关闭 |
| 恢复 | 软件重启UART ISP及刷写工具 | 已验证 | 手动BOOT/RESET仍是兜底 |
| 板载UI | RGB连接状态灯 | 已验证 | 绿色空闲、蓝色连接中/已连接 |

## 正式默认值

| 设置 | 默认 |
| --- | --- |
| CPU | 320 MHz、manual governor |
| 编译期超频 | 关闭 |
| 麦克风处理 | 关闭 |
| 扬声器路由 | Auto：未插3.5 mm时mono，插入耳机时stereo |
| USB Audio OUT | 四通道48 kHz/16-bit |
| Profiling | 关闭 |
| 串口报告日志 | 可切quiet/normal |

默认配置优先保证扬声器/震动连续。开启麦克风解码会增加第二个Opus负载；全双工功能可用，
但320 MHz下的主观抗卡顿余量还没有达到speaker-only水平。性能优化不得降低采样率、
位宽、码率、声道、帧长或频带。

## 尚未实现

这些是真实产品缺口，不是隐藏编译选项：

- Pico固件的WebHID配置报告`0xF6`–`0xF9`；
- 覆盖音频路由、mic状态、轮询模式、手柄模式、USB序列号、唤醒、快捷键、增益和空闲
  时间的版本化统一持久配置；
- 浏览器触发固件升级/USB DFU（M61当前走UART ISP）；
- 可选USB HID轮询率及DualSense/DualSense Edge模式；
- Windows唤醒/Game Bar快捷键模拟；
- 固件侧haptics/扬声器增益及自适应扳机削减配置；
- Web可读的实时性能计数与DVFS/音频控制；
- DualSense Edge（`054C:0DF2`）全链路真机支持；
- 默认320 MHz下长时间全双工零可闻卡顿验收。

Web迁移计划以及固件/Web职责划分将写在独立config-web仓库的重构spec中。

## 串口命令面

运行时以`ds5 help`和`m61 help`为权威。主要命令组：

- `ds5 status`、`scan`/`pair`、`connect`、`autoconnect`、`disconnect`、
  `forget`、`security`、`sdp`、`hidp`、`bringup`；
- `ds5 mic on|off`和`ds5 speaker auto|mono|stereo`；
- `ds5 get-feature`、`output-init`、`send-ctrl`、`send-intr`用于协议诊断；
- `m61 clock status|profile|lock|governor|boost|save|clear-saved`；
- `ds5 reboot-isp` / `m61 reboot-isp`。

Raw send和benchmark命令属于开发工具，不应直接变成浏览器公共API。
