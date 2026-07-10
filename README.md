# DS5Dongle(双芯片版)

把 DualSense(PS5 手柄)通过经典蓝牙桥接为 PC 上的原生 DualSense USB 设备。
双芯片架构:

```
DualSense ⇄(经典蓝牙 HIDP 0x11/0x13)⇄ ESP32 ⇄(SPI 私有帧协议)⇄ M61/BL618 ⇄(原生 USB)⇄ PC
```

- **ESP32**:蓝牙侧。运行 [BTstack](https://github.com/bluekitchen/btstack)
  (裁剪版内置于 `components/btstack`,VHCI 直连内置 controller,无需 patch ESP-IDF)。
  蓝牙状态机以参考项目 [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)
  `ea93fad59a8f74e49f649a59005dc8b1a6b87a70` 的 `src/bt.cpp` 为准，按 ESP32
  VHCI 和双板线程模型做必要适配。
- **M61(Ai-M61-32S / BL618)**:USB 侧。CherryUSB 枚举为 `VID_054C&PID_0CE6`
  DualSense 复合设备(HID + USB Audio),bl_mcu_sdk 构建。
- 两芯片间为自定义 SPI 帧协议(`main/dual_chip_spi_proto.h`,M61 为 SPI master)。

## 目录结构

| 路径 | 内容 |
|------|------|
| `main/` | ESP32 固件:`bt_ds5_btstack.c`(蓝牙状态机)、`esp32_dual_chip_spi.c`(SPI 从机桥)、`dual_chip_spi_proto.*`(帧协议) |
| `components/btstack/` | 裁剪版 BTstack(仅经典蓝牙 host + VHCI port + NVS link key) |
| `m61/dualsense_hidp_probe/` | M61/BL618 固件(USB 复合设备 + SPI master + 私有配置协议) |
| `tools/` | 构建/烧录/日志检查脚本 |
| `docs/` | 现行文档;`docs/archive/` 为历史资料(不可尽信) |

## 蓝牙行为

- **首次配对**:手柄按住 PS+Create 进入配对模式 → ESP32 inquiry 扫描
  (按 CoD 手柄类别 `0x000500` 掩码过滤)→ 创建 ACL → SSP 认证/加密 →
  主动打开 L2CAP HID Control(PSM 0x11)与 Interrupt(PSM 0x13),MTU 672。
- **回连**:手柄单按 PS 主动 page 回连,ESP32 按已注册的 L2CAP service 接受;
  link key 由 BTstack 持久化在 NVS。ESP32 保持 page scan 的同时继续 inquiry,
  所以旧 bond 失效后仍可用 PS+Create 重新配对。
- **地址保存**:连接成功后手柄地址写入 NVS(`ds5bt/saved_bda`),
  供状态上报和识别回连来源;真正的认证凭据是 BTstack NVS link key。
  `BT_FORGET` 命令可分别清除绑定与地址。
- 输出报文:`0xA2` 前缀 + CRC32(seed 同参考 `utils.h`);
  Feature:`0x43`(GET)/`0x53 + CRC32`(SET);
  连接建立后预取 0x09/0x20/0x22/0x05 并用 0x70 探测 DualSense Edge。

## 私有配置协议(USB HID Feature Report)

`0xF6`~`0xF9` 的 HID 线格式已按上游 `ea93fad` 对齐。`Config_body` 固定为
20 字节，`mic_select/speaker_select` 均为 `0..3`;EasyFlash 中保存上游兼容的
`magic + CRC32 + size + body` 32 字节封装，并自动迁移旧版 20 字节布尔配置。
字段布局、CRC 向量和仍待实现的高级功能见 `docs/PRIVATE_PROTOCOL.md`。

| Report | 方向 | 含义 |
|--------|------|------|
| `0xF7` | GET | 返回 `Config_body`(触觉增益、轮询率、控制器模式等) |
| `0xF8` | GET | 固件版本字符串 |
| `0xF9` | GET | 字节0:蓝牙 RSSI;字节1:音频通路状态(bit7 有效标志) |
| `0xF6` | SET | 子命令 `0x01` 更新内存配置 / `0x02` 写入 flash / `0x03` USB 重枚举 |

## 构建

ESP32(Windows,ESP-IDF v5.3.2 位于 `C:\tmp\esp-idf-v5.3.2`):

```
python tools/build_esp32_stage1.py --backend dual-chip --pin-profile devkit-left
```

产物 `build_dual_chip_devkit_left/ds5_dualsense_bridge_esp32.bin`。

M61(WSL,T-Head 工具链 `/opt/toolchain_gcc_t-head_linux`):

```
bash tools/build_m61.sh dual     # 双芯片 SPI 配置(defconfig.dual_chip)
bash tools/build_m61.sh single   # 单芯片蓝牙直连配置(defconfig)
```

产物 `m61/dualsense_hidp_probe/build/build_out/m61_dualsense_hidp_probe_bl616.bin`。

## 接线与 LED

引脚见 `docs/DUAL_CHIP_WIRING.md`(M61 IO13/11/10/20 ↔ ESP32 GPIO27/26/25/33,
READY IO16↔GPIO32,IRQ IO17↔GPIO13);M61 原生 USB 注意事项见
`docs/M61_NATIVE_USB_WIRING.md`(电脑必须接 BL618 原生 `USB_DP/USB_DM`,
板载 CH340 口只是串口)。

- ESP32 蓝灯:闪烁=扫描/连接中,常亮=手柄已连接。
  开机 M61 自检(wire-test)期间会短暂闪烁,通过后显示 3 秒结果再回到蓝牙状态。
- M61 RGB:绿=待机(SPI 尚未握手),蓝闪=等待手柄(ESP32 已就绪),蓝常亮=手柄已连接。
  开机 wire-test 通过显示 3 秒绿色后回到自动状态;失败保持红色(检查接线)。
- M61 收到完整 0x31 输入报文后才挂载 USB(电脑此时才枚举出手柄)。

## 排障

1. **扫描配对**:手柄按住 PS+Create 至灯条快速双闪 → ESP32 串口应出现
   `Inquiry result event=...` →
   `Gamepad found` → `ACL connected` → `Authentication complete status=0x00` →
   `HID Control/Interrupt opened`。
2. **PS 单按回连**:串口应出现 `Incoming ACL` → `Link key reply` →
   `HID Interrupt opened`;若出现 `No link key ... force re-pair`,
   说明 NVS 中无该手柄绑定,需重新进入配对模式。
3. **无 USB 设备**:确认 M61 串口出现
   `DualSense full report seen via ESP32; starting USB composite device`;
   若无,先看 ESP32 是否已 `HID Interrupt opened`,再看 SPI 是否有
   `SPI rx valid` / `BT_RX_INPUT` 流量。
4. **持续 `HIDP tx from SPI failed err=-128`**:表示蓝牙 interrupt 通道未打开,
   M61 在旧固件上会盲发输出报文;两侧都刷新到本分支固件后不应再出现。

> 构建注意:在 MSYS/Git-Bash 环境直接调用 `idf.py` 会因 `MSYSTEM`
> 环境变量被 idf.py **静默跳过构建**(只打印警告并返回 0)。请在
> cmd/PowerShell 中构建,或先 `set MSYSTEM=`。`tools/build_esp32_stage1.py`
> 需在 cmd 下运行。

## 文档

- `docs/REBUILD_PLAN.md` — 本次重建的方案与验收标准
- `docs/PRIVATE_PROTOCOL.md` — 0xF6~0xF9、Config_body 与 flash 格式
- `docs/AUDIO_PIPELINE.md` — 48 kHz/480 帧直通与上游 512→480 差异
- `docs/DUALSENSE_REPORT_31.md` — DualSense 0x31 报文结构参考
- `docs/archive/` — 旧阶段文档,仅供考古,内容与现状可能不符
