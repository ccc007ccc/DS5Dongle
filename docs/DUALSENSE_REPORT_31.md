# DualSense 0x31 输入报文

这是当前 M61 主线和 ESP32 fallback 共享的 DualSense 输入解析约定。M61 Classic Bluetooth HIDP 路线以 `report=0x31 mode=full` 作为完整质量输入门槛，当前实现接受三种输入形态：

- `A1 31 <seq> <payload...>`：完整 BT HIDP Input 数据。
- `31 <seq> <payload...>`：HID Host 已去掉 HIDP transaction byte。
- `<payload...>`：HID Host 已去掉 HIDP transaction byte、report id 和 sequence。

实机 bring-up 中还可能先出现 `01 <first 9 bytes of common input...>` 的 10 字节短输入报文。当前实现会解析其中的摇杆、扳机、方向键和基础按键，并在日志里标为 `report=0x01 mode=basic`。这只证明 HID 输入路径可用；正式 M61 主线路径仍要求 `report=0x31 mode=full`，因为 0x01 短报文没有 IMU、电量和完整状态字段。

M61 原始 L2CAP HIDP 探针可能收到 `A1 01 <first 9 bytes...>`，其中 `A1` 是 HIDP DATA/Input transaction byte。解析器也接受这个形态，仍标为 `report=0x01 mode=basic`。

`payload` 的字段偏移如下：

| Offset | 字段 |
| --- | --- |
| 0 | LeftStickX |
| 1 | LeftStickY |
| 2 | RightStickX |
| 3 | RightStickY |
| 4 | L2 trigger |
| 5 | R2 trigger |
| 7 low nibble | D-pad |
| 7 high nibble | Square/Cross/Circle/Triangle |
| 8 | L1/R1/L2/R2/Create/Options/L3/R3 |
| 9 | PS/Touchpad/Mute/Edge buttons |
| 15..20 | Gyro X/Z/Y, signed little-endian |
| 21..26 | Accelerometer X/Y/Z, signed little-endian |
| 27..30 | Sensor timestamp, little-endian |
| 31 | Temperature |
| 52 low nibble | Battery percent, 0..10 |
| 52 high nibble | Power state |
| 53 | Headphone/mic/USB flags |

麦克风/音频负载不属于当前手柄输入状态解析。如果头部 sequence/flags 字节的 bit 1 为 1，固件只记录调试日志并跳过。
