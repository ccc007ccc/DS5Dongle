# 协议与音频格式

[English](PROTOCOL.md)

本文描述M61固件实际使用的格式，不是完整Sony协议规范。

## USB身份

| 字段 | 值 |
| --- | --- |
| Vendor/Product | `054C:0CE6` |
| 产品字符串 | `DualSense Wireless Controller` |
| HID interface | 3 |
| HID IN/OUT | `0x84` / `0x03` |
| Audio OUT/IN | `0x01` / `0x82` |

复合描述符包含Audio Control、Audio Streaming OUT、Audio Streaming IN和HID接口。

## 输入报告

蓝牙完整输入使用`0x31`。解析器接受带或不带`0xA1`事务字节的HIDP报告，bring-up期间
也能解析短`0x01`。只有`0x31`包含正式路径需要的完整触摸、IMU、电量和设备状态。

M61先把common payload转换成有线USB输入布局再发给电脑；蓝牙CRC和transport header
不会作为USB输入数据暴露。

## Output与Feature报告

USB output state被合并到手柄63-byte蓝牙状态，并按需通过`0x31`/`0x32`/`0x36`发送。
CRC32使用reflected polynomial `0xEDB88320`；release使用位精确16项nibble表。

USB Feature GET/SET是透明代理：

- 返回USB buffer的byte 0保留Report ID；
- 多个GET走有界FIFO并合并重复请求；
- SET先于依赖它的GET转发；
- SET `0x80`会使动态GET `0x81`失效并刷新；
- Bluetooth link或USB session重置时清空cache。

因此普通DualSense主机软件可以读取手柄固件、工厂、MAC和telemetry页面。

## USB Audio OUT

| 属性 | 值 |
| --- | --- |
| 采样率 | 48,000 Hz |
| 位宽 | signed 16-bit little-endian PCM |
| 声道 | 4，交错 |
| Channel 0/1 | speaker/headset左、右 |
| Channel 2/3 | HD-haptics左、右 |
| 标称USB包 | 每1 ms 384 bytes |
| 描述符最大包 | 392 bytes |

Speaker PCM以512帧epoch收集并转换为480-sample、10 ms Opus帧。release encoder为
fixed-point、CBR、complexity 0、medium-band、160 kbit/s，按运行时路由强制1或2声道；
编码块pad到200 bytes供手柄传输。这些参数属于质量/协议不变量。

Haptics声道被降采样并转换为手柄signed 8-bit左右64-byte block。两个相邻epoch打包进
实时Bluetooth audio report。

## 手柄麦克风与USB Audio IN

| 属性 | 值 |
| --- | --- |
| 蓝牙payload | 71-byte Opus，已观察到固定TOC `0xD4` |
| Decoder采样率 | 48,000 Hz |
| Decoder声道 | 1 |
| 帧 | 480 samples / 10 ms |
| USB位宽 | signed 16-bit little-endian PCM |
| USB声道 | 2；mono样本复制到L/R |
| 标称USB包 | 每1 ms 192 bytes |
| 描述符最大包 | 196 bytes |

D4快路只处理完全匹配的已知包型。PLC、FEC、不同TOC/长度/采样率/buffer契约都会立即
回退upstream Opus parser。Host测试已证明baseline与fast-path PCM逐字节一致。

## 扬声器路由

- `auto`：手柄报告插入3.5 mm耳机时用stereo headset block `0x16`，否则使用mono
  controller-speaker block `0x13`；
- `mono`：downmix speaker声道并用`0x13`；
- `stereo`：保留L/R并用`0x16`。

路由切换时，主机侧四声道USB格式始终不变。

## M61 WebHID管理协议

M61配置器使用四个供应商Feature Report。这是M61原生版本化协议，不暴露固件私有
内存布局：

- `0xF6`：应用/保存配置、USB重连、关闭手柄、配对、断开及忘记命令；
- `0xF7`：schema v4配置与能力位，包含左右独立的缩放径向摇杆死区和USB回报率模式；
- `0xF8`：固件和产品身份；
- `0xF9`：telemetry v2连接、运行时、管理结果与受限健康计数；前8个payload字节
  保持兼容telemetry v1。

21-byte配置体以带CRC32的EasyFlash记录保存。记录无效时回退正式默认值：麦克风和
超频关闭、manual 320 MHz、空闲关机关闭、左右死区均为0%，USB实时转发新蓝牙样本。
v1到v3记录迁移时采用该实时模式。
