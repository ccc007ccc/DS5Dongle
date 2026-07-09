# DS5Dongle 双芯片方案重建计划(2026-07-09)

## 背景与结论

当前 `m61-esp32-dual-chip` 分支的 ESP32 蓝牙实现基于 bluedroid 的 `esp_bt_l2cap_*`
(BTA JV/OBEX 路径),并依赖 `tools/patch_esp_idf_hidp_l2cap.py` 直接修改 ESP-IDF
源码把 ERTM 强改为 Basic mode。该路径从设计上就不适合 HID 主机角色:

- `esp_bt_l2cap_*` 走 VFS 文件描述符收发,延迟与线程模型都不适合 8ms 级 HID 流;
- 需要 patch IDF 才能编译/运行,不可维护;
- 配对 / link key / 自动重连语义与 DualSense 期望(SSP + 手柄回连 page)不匹配。
- 实测表现:手柄灯常亮(ACL 已连)但 HIDP 通道打不开,ESP 一直闪蓝灯,
  M61 因收不到 `BT_STATE_FULL_REPORT` 不放行 USB,电脑无设备。

**结论:ESP32 侧蓝牙整体重建,换用 BTstack(官方 ESP32 VHCI port),
按参考项目 [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)(Pico W + BTstack)
的 `src/bt.cpp` 状态机 1:1 移植。** 参考仓库已 clone 到 `C:\code\MCU\DS5Dongle_ref`。

## 参考实现要点(awalol/DS5Dongle,必须还原)

### 蓝牙状态机(src/bt.cpp)
- `bt_init()`:SSP enable + secure connections + IO cap `DISPLAY_YES_NO` +
  authreq `MITM_NOT_REQUIRED_GENERAL_BONDING`;page scan 11.25ms interlaced;
  connectable + discoverable;HCI power on。
- `bt_l2cap_init()`:`sdp_init()`(重连后自动断开的关键修复)+
  `l2cap_register_service(PSM 0x11/0x13, MTU 672, LEVEL_2)` + `l2cap_init()`。
- 首配:inquiry 30s → CoD `(cod & 0x000F00) == 0x000500` 判定手柄 →
  `hci_create_connection` → `hci_authentication_requested` →
  link key 请求(有 key 回 key,无 key 负回复强制重配)→
  SSP user confirmation 自动接受 → 认证完成后 `hci_set_connection_encryption` →
  加密成功且 `new_pair` 时主动 `l2cap_create_channel`(先 0x11 后 0x13)。
- 回连:手柄发起 page,`HCI_EVENT_CONNECTION_REQUEST` 按 CoD accept,
  L2CAP incoming connection 直接 accept,通道由手柄经已注册 service 建立。
- link key 持久化:BTstack link key DB(TLV);ESP32 port 对应
  `btstack_tlv_esp32`(NVS)+ `btstack_link_key_db_tlv`。
- 断开:清 cid/handle/发送队列,回 `state_init_data`(66 字节摇杆中位报文)
  给 USB 侧,重新 connectable/discoverable。
- 发送:interrupt 通道走 10 深度 FIFO + `L2CAP_EVENT_CAN_SEND_NOW`;
  输出报文 `0xA2` 前缀 + CRC32 尾(`fill_output_report_checksum`)。
- Feature:control 通道 `0x43 id` GET / `0x53 id + payload + CRC32` SET,
  `0xA3` 开头的响应按 report id 缓存;`init_feature()` 预取 0x09/0x20/0x22/0x05,
  并用 0x70 探测 DSE。
- 连接建立后发 `update_state()` 点亮手柄 LED。

### 私有协议(USB HID Feature Report,必须逐字节一致)
- `0xF7` GET → 返回 `Config_body`;`0xF8` GET → 固件版本串;`0xF9` GET → RSSI;
- `0xF6` SET,`buffer[0]` 子命令:`0x01` set_config、`0x02` config_save、
  `0x03` tud_disconnect/tud_connect。
- Flash `Config`:`magic 0x66ccff00` + `version` + `crc32(body)` + `size` + body。

## 保留 / 重建边界

| 模块 | 处置 |
|------|------|
| `main/dual_chip_spi_proto.*`(SPI 帧协议) | 保留 |
| `main/esp32_dual_chip_spi.c`(SPI 从机桥) | 保留,对接新 BT 层;跨线程调用经 `btstack_run_loop_execute_on_main_thread` |
| `main/bt_dualsense_raw_hidp.c`(bluedroid raw HIDP) | 删除,由 `bt_ds5_btstack.c` 替代(API 头文件保留兼容) |
| `main/bt_dualsense_host.c`(HID Host 后端) | 删除 |
| `tools/patch_esp_idf_hidp_l2cap.py` | 删除(不再 patch IDF) |
| `m61/dualsense_hidp_probe`(BL618 USB 侧) | 保留,私有协议按参考逐字节核对修正 |
| 根目录 *.log | 移入 `logs/archive/` |
| docs/* 旧文档 | 归档到 `docs/archive/`,重写核心文档 |

## 实施阶段(每阶段一次 git 提交)

1. **阶段0**:本计划文档 + 仓库清理(日志、过期文档归档)。
2. **阶段1**:BTstack 以项目组件 `components/btstack` 集成;
   `sdkconfig.dual_chip` 切 Controller-Only(VHCI);模板初始化可编译。
3. **阶段2**:`main/bt_ds5_btstack.c` 按 bt.cpp 移植全部状态机,
   实现 `bt_dualsense_raw_hidp.h` 既有 API(connect/disconnect/forget/
   send_report/get_feature/set_feature/state 回调);
   地址保存:link key 走 BTstack TLV-NVS,最近手柄地址另存 NVS 供状态上报。
4. **阶段3**:SPI 桥对接 + `idf.py build`(Windows IDF v5.3.2)通过。
5. **阶段4**:M61 私有协议对齐(0xF6~0xF9、Config magic/CRC、
   tud_connect 门控与 `state_init_data` 行为);WSL riscv 工具链构建通过。
6. **阶段5**:重写 README/docs(架构、接线、构建、烧录、配对/回连流程),
   删除失效文档,收尾提交。

## 构建环境

- ESP32:Windows 侧 ESP-IDF v5.3.2(`C:\tmp\esp-idf-v5.3.2`),
  `SDKCONFIG_DEFAULTS=sdkconfig.dual_chip.defaults`,build 目录 `build_dual_chip`。
- BL618:`C:\code\MCU\bl_mcu_sdk`,WSL Ubuntu 24.04 `riscv64-unknown-elf-gcc`。

## 验收标准

- 手柄按 PS+Share 可被扫描配对,ESP32 打开 0x11/0x13 通道并收到 0x31 输入报文;
- 断电重启后手柄单按 PS 能自动回连(link key 持久化生效);
- M61 收到 `FULL_REPORT` 状态后 `tud_connect`,Windows 枚举出 DualSense;
- 私有协议 0xF6~0xF9 与参考项目 Web 配置工具兼容;
- 全程无需 patch ESP-IDF。
