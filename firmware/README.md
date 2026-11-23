# FakeTec LoRa 串口桥固件

基于 `faketec.kicad_sch`（fakeTec v5：nice!nano / ProMicro nRF52840 + Ra-01S LoRa + I2C OLED）的透明串口桥固件。

- LoRa 点对点双向串口透传，默认 433.000 MHz，频率/带宽/SF/CR/功率等全部可配置
- 两台设备一键配对，配对后：跳频 (FHSS) + 自适应信道屏蔽 (AFH) + 先听后发 (LBT) + ACK 重传 + AES-128 加密
- USB 为独立的 CDC 虚拟串口（数据口）
- 蓝牙：NUS 蓝牙串口（数据口）+ 配置服务 + 电池服务 + OTA DFU
- 保留 UF2 / 串口 DFU（Adafruit nRF52 bootloader），兼容 Meshtastic / RNode 刷机工具
- OLED 显示连接状态、收发速率、RSSI/SNR、电池余量等

## 硬件引脚（来自原理图）

| 功能 | nRF52840 | 说明 |
|---|---|---|
| LoRa NSS / SCK / MOSI / MISO | P1.13 / P1.11 / P1.15 / P0.02 | SPI |
| LoRa RST / DIO1 / BUSY | P0.09 / P0.10 / P0.29 | P0.09、P0.10 已配置为 GPIO（非 NFC） |
| LoRa RXEN | P0.17 | TX 开关由 DIO2 控制 |
| OLED SDA / SCL | P1.04 / P0.11 | SSD1306 或 SH1106，地址 0x3C/0x3D 自动探测 |
| 按键 SW2 (D6) | P1.00 | 低有效 |
| 电池分压 (10M/10M) | P0.31 / AIN7 | ×2 |
| LED | P0.15 | nice!nano 板载 |
| 3.3V 外设电源使能 | P0.13 | nice!nano VCC 开关，固件上电置高 |
| MOSFET T1/T2/T3 | P0.24 / P0.08 / P0.06 | `AT+OUT=n,v` 控制 |

LoRa 模块：Ra-01S（SX1268，410–525 MHz）。实测该板上通过 DIO3 控制 TCXO 的各档电压（1.6–3.3 V）均初始化失败，晶振模式正常，因此默认 `TCXO=0`；如更换为需要 DIO3 供电 TCXO 的模块（如 HT-RA62），设置 `AT+TCXO=1.8`。TCXO 失败时会自动回退晶振模式。芯片版本字符串为 `SX1261 V2D 2D02`，固件会自动接受（RadioLib 默认只认 `SX1268` 会报 -2）。
如改用 HT-RA62 / Ra-01SH（SX1262），在 `platformio.ini` 的 `build_flags` 加 `-DLORA_CHIP_SX1262`。

## 编译

```
pio run
```

输出在 `.pio/build/faketec/`：

| 文件 | 用途 |
|---|---|
| `firmware.uf2` | 拖进 bootloader 盘符（Meshtastic Web Flasher 的 nRF52 方式） |
| `firmware.zip` | Nordic 旧版 DFU 包，与 `adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req 0x00B6` 输出逐字节一致；用于 rnodeconf / RNode Flasher、adafruit-nrfutil 串口 DFU、nRF Connect / Bluefruit 蓝牙 OTA |
| `firmware.hex` | SWD 烧录 |

要求板子上是 nice!nano / Adafruit nRF52 UF2 bootloader + S140 6.1.1（ProMicro nRF52840 出厂即是，Meshtastic DIY 板也是这个组合）。

## 刷机 / 进入 DFU

以下方式都会进入 bootloader（同时提供 UF2 盘符和串口 DFU）：

1. **1200 bps touch**：以 1200 波特率打开再关闭 USB 串口（Meshtastic Flasher、rnodeconf、`pio run -t upload` 都会自动这么做）
2. 双击 RST 按键
3. 命令 `AT+DFU`（`AT+OTADFU` 进入蓝牙 OTA DFU 模式）

示例：

```
# UF2：进入 DFU 后把 firmware.uf2 复制到出现的 NICENANO / FTHR840BOOT 盘
# 串口 DFU（与 RNode 安装方式相同）：
adafruit-nrfutil dfu serial --package firmware.zip -p COMx -b 115200 -t 1200 --singlebank
# PlatformIO：
pio run -t upload
```

> 注意：因为 1200 bps touch 是 DFU 触发条件，数据口请不要用 1200 波特率打开（USB CDC 的波特率设置对透传无影响，任何其他值都可以）。

## 使用

### 数据透传

- USB 串口和蓝牙 NUS 串口都是数据口：从任一口收到的数据经 LoRa 发给对端，对端收到后同时输出到它的 USB 和蓝牙串口。
- 发送打包：凑满 `MAXPL` 字节（默认 200）或串口空闲 `PKTTO` 毫秒（默认 20）即发出一帧。
- USB 有流控：LoRa 发送队列（4 KB）满时停止从 USB 读取，主机端会被 NAK 阻塞，不丢数据。
- 未配对时为广播模式：在基频上无 ACK 广播，任何未配对设备都可收（便于调试）。

### 配对

1. 两台设备设置相同的射频参数（出厂默认即可）
2. 两台都长按按键 3 秒后松开（或发 `AT+PAIR`），进入 60 秒配对模式（LED 闪烁）
3. 互相发现后自动交换 ID 和随机 AES 密钥并保存，屏幕显示对端 ID

长按 10 秒松开 = 取消配对（`AT+UNPAIR`）。也可用 `AT+PEER` + `AT+KEY` 手动配置两端。

### 按键

| 操作 | 功能 |
|---|---|
| 短按 | 唤醒屏幕 / 翻页（主页、链路统计、配置）/ 取消配对 |
| 长按 3 s | 配对 |
| 长按 10 s | 解除配对 |

### 省电待机与链路状态

屏幕标题栏（以及 `ATI` 的 `Mode`、`AT+ST`）显示链路状态，后面的 `M` / `S` 表示主机 / 从机：

| 状态 | 含义 |
|---|---|
| `UNPR` | 未配对（广播模式） |
| `STBY` | 已配对、省电待机，能收到对端心跳 |
| `LINK` | 链路已激活，可传数据 |
| `SRCH` | 正在唤醒对端（最多约 8 秒） |
| `NOOP` | 心跳或唤醒都没有发现对端 |
| `PAIR` | 配对中 |

配对后默认处于 **STBY**：LoRa 接收机占空比工作（大部分时间休眠），停在基础信道，只按 `AT+SBHB`（默认 30 秒）发一次心跳。
只有**两端都有串口在用**（USB 插入主机、WebUSB 客户端接入，或蓝牙串口已连接）时，双方才进入 **LINK**（接收常开、恢复跳频、开始传数据）。先就绪的一端每 4 秒用长前导码通告一次，另一端就绪后约 1~5 秒建立链路。
任意一端的 USB 拔出（按 VBUS 检测）或蓝牙断开后，两端都在约 2 秒后回到 STBY（通告丢失时最长约 13 秒）。
`AT+PSM=0` 可关闭省电，链路始终保持激活。

### 客户端与带内控制通道

- `client-rust/`：命令行客户端 `lbridge`（终端 + 配置），见其 README。
- `client-web/`：WebUSB 网页客户端（终端 + 配置），见其 README。

USB 串口、WebUSB、蓝牙串口三个端口都支持带内控制通道，任何时候可用，不需要 `+++` 和等待时间：

```
主机 -> 设备 : ESC ] L B ; <AT 命令> BEL
设备 -> 主机 : ESC ] L B ; <应答文本> BEL
```

`AT+ST` 返回一行机器可读状态：`+ST=状态,主从,RSSI,SNR,电量,USB,发送速率,接收速率,对端电量,队列,信道,频率,对端RSSI,对端SNR`。

### 蓝牙配对

蓝牙串口和配置服务需要加密配对。手机发起配对时屏幕显示 6 位配对码，核对与手机一致后**短按按钮确认**（长按或 30 秒不操作则拒绝）。
配对信息会保存，之后重连无需再确认；`AT+BLEUNBOND` 清除已配对的手机，`AT+BLEPAIR=0` 关闭配对要求。

### 数据压缩

数据帧在加密前做 LZ 压缩，一帧最多可装 512 字节原始数据，文本类数据（日志、NMEA、AT 指令等）通常能减少 30%~60% 的空中数据量。`AT+STAT` 的 `Compression` 行显示实际压缩比。

### 屏幕

- 标题栏：链路状态和主从（如 `LINK M`、`STBY S`）、收发指示 `>` `<`、`BT` 蓝牙已连接、`U` USB 串口已打开、`C` 命令模式、电池 % 与图标
- 第 1 页（状态动画）：用字符画表示本机、对端和两者之间的链路，下面两行是状态说明和速率 / 频率

  ```
   .---.         .---.
   | M |=>=>=>=>=| S |
   '---'         '---'
  RSSI -47 SNR 12.5
  TX 480B/s  RX 0B/s
  ```

  | 状态 | 动画 |
  |---|---|
  | `LINK` | 有数据时箭头沿传输方向流动（`=>=>` 发送、`<=<=` 接收、双向时左右各一半）；空闲时一个 `o` 在连线上来回移动 |
  | `STBY` | 两个方框之间依次出现 `z Z z` |
  | `SRCH` | `) ) ) )` 从本机向对端扩散 |
  | `NOOP` | 对端方框变成虚线，中间 `x` 闪烁 |
  | `UNPR` | 对端方框为虚线，中间 `?` 闪烁，提示长按 3 秒配对 |
  | `PAIR` | `<-  ->` 向中间合拢，显示剩余秒数 |

- 第 2 页：当前频率/SF/BW、对端 ID 与跳频信道、RSSI/SNR、**TX/RX 速率**、队列/重传/丢包
- 第 3 页：本机 ID、主从角色、对端看到的本机信号与对端电量、帧计数、CRC/重复/LBT 次数、跳频次数、屏蔽信道掩码
- 第 4 页：功率/CR/前导、跳频参数、ACK/加密/AFH、蓝牙名、电池电压

`AT+OLED=秒` 设置息屏时间（0 = 常亮，默认 120）。

省电：链路未激活时（`STBY` / `NOOP` / `UNPR`）屏幕每 2 秒才刷新一帧，并且最多 30 秒后息屏（设置为常亮时除外）；`LINK` 和配对时每秒 4 帧，按设置的时间息屏。短按按钮唤醒。
每次开机固件会先把外设 3.3V 电源（P0.13）切断 0.6 秒再上电，对 OLED 和 LoRa 模块做一次硬复位。接着电池时 OLED 不会随复位/刷机掉电，控制器一旦卡死（仍应答 I2C 但画面停在旧固件的残影）只能靠断电恢复。

## 配置

### 命令模式

- USB 或蓝牙串口：静默 1 秒 → 发送 `+++` → 静默 1 秒，返回 `OK` 进入命令模式；`ATO` 回到透传（5 分钟无操作自动返回）
- 蓝牙配置服务（不影响蓝牙串口透传）：
  - Service `8A9B0001-3C4D-4E5F-8061-7283A4B5C6D7`
  - `…0002…` 写入 AT 命令（每次写入一条）
  - `…0003…` 订阅通知获取响应

可以用 nRF Connect、LightBlue 或任意 BLE 串口 App 配置。

### AT 命令

| 命令 | 默认 | 说明 |
|---|---|---|
| `AT` / `ATI` / `AT+HELP` / `AT+CFG` | | 测试 / 信息 / 帮助 / 全部参数 |
| `AT+STAT` / `AT+RSTAT` | | 链路统计 / 清零 |
| `AT+FREQ=433.000` | 433.000 | 基频（同步信道）MHz |
| `AT+BW=125` | 125 | kHz：7.8 10.4 15.6 20.8 31.25 41.7 62.5 125 250 500 |
| `AT+SF=7` `AT+CR=5` | 7 / 5 | 扩频因子 5–12，编码率 4/5–4/8 |
| `AT+PWR=17` | 17 | dBm，-9..22（请遵守当地法规） |
| `AT+PRE=8` `AT+SYNC=12` `AT+TCXO=1.8` | | 前导码、同步字(hex)、TCXO 电压 (0 = 晶振) |
| `AT+HOP=1` | 1 | 配对后启用跳频 |
| `AT+HOPCH=8` `AT+HOPSP=200` | 8 / 200 | 跳频信道数、信道间隔 kHz（信道 i = 基频 + i×间隔） |
| `AT+DWELL=0` | 0 | 每跳驻留时间 ms，0 = 按空中时间自动计算 |
| `AT+AFH=1` | 1 | 自适应信道屏蔽 |
| `AT+LBT=1` `AT+LBTRSSI=0` | 1 / 0 | 先听后发；附加 RSSI 门限 dBm（0 = 关） |
| `AT+ACK=1` `AT+RETRY=4` | | ACK 确认与重传次数 |
| `AT+ENC=1` | 1 | 配对后 AES-128-CTR 加密 |
| `AT+MAXPL=200` `AT+PKTTO=20` `AT+HB=3` | | 每帧最大负载、打包超时 ms、心跳 s |
| `AT+PAIR[=60]` `AT+UNPAIR` `AT+PEER=hex` `AT+KEY=hex32` | | 配对相关 |
| `AT+BLE=1` `AT+NAME=xxx` `AT+BLEPIN=0` | | 蓝牙开关、名称、6 位配对 PIN（0 = 无），重启生效 |
| `AT+OLED=120` `AT+OLEDTYPE=0` `AT+FLIP=0` | | 息屏时间、0=SSD1306/1=SH1106、旋转 180° |
| `AT+PSM=1` `AT+SBHB=30` `AT+USBACT=1` | | 省电待机开关、待机心跳间隔（秒）、USB 激活条件（1=插入主机，0=串口被打开） |
| `AT+BLEPAIR=1` `AT+BLEUNBOND` | 重启生效 | 蓝牙需按钮确认配对；清除所有已配对手机 |
| `AT+COMP=1` | | 数据帧 LZ 压缩（逐帧独立，压不动的数据自动原样发送） |
| `AT+BATCAL=1000` `AT+BAT?` | | 电池电压校准系数 ×1000、读电池 |
| `AT+I2C` `AT+HW` | | 扫描 I2C 总线；硬件诊断（GPIO 电压、Bootloader/SoftDevice 版本、OLED 探测与开机断电复位结果） |
| `AT+OUT=n,v` | | MOSFET T1..T3 开关 |
| `AT+DEFAULT` `AT+REBOOT` `AT+DFU` `AT+OTADFU` | | 恢复出厂、重启、进 UF2/串口 DFU、进蓝牙 OTA DFU |

查询：`AT+XXX?`。修改立即生效并保存到内部 Flash（LittleFS）。**两端的射频参数和跳频参数必须一致。**

## 抗干扰设计

| 机制 | 说明 |
|---|---|
| 跳频 FHSS | 配对后在 `HOPCH` 个信道间按时隙跳频，跳频序列由配对密钥和双方 ID 生成的伪随机排列决定。ID 小的一方为时钟主机，每一帧都携带发送方时钟，从机据此持续校准（RX 时间戳取自中断）。时隙首尾各留 40 ms 保护，帧只在时隙剩余时间足够“数据 + ACK”时发出 |
| 失步恢复 | 超过 `3×HB+…` 秒没收到对端：双方回到基频（信道 0），主机每秒发 SYNC 信标，从机回 SYNC_ACK 后重新进入跳频 |
| 自适应屏蔽 AFH | 每 10 s 统计各信道 ACK 丢失率、CRC 错误和底噪（与中位数比较 +12 dB），坏信道屏蔽 60 s；从机把自己的统计放在心跳里上报，主机合并后下发，跳频时坏信道由序列中下一个好信道替代；至少保留一半信道，信道 0 不屏蔽 |
| 先听后发 LBT | 发送前检查前导码/报头检测标志（可选 RSSI 门限），信道忙则随机退避 |
| ARQ | 停等式 ACK、可配置重传次数、随机退避、序号去重 |
| 过滤 | 私有同步字 + 帧头魔数 + 目的地址过滤 + LoRa CRC |
| 加密 | AES-128-CTR（nRF52 硬件 ECB），nonce = 源/目的 ID + 序号 + 时间戳 |

## 帧格式

```
| type(1) | flags(1) | src(4) | dst(4) | seq(2) | time(4) | payload(0..239) |
type: 0xB0 | {1 DATA, 2 ACK, 3 PING, 4 SYNC, 5 SYNC_ACK, 6 PAIR_REQ, 7 PAIR_ACK}
flags: bit0 加密, bit1 需要 ACK
```

## 已知限制

- 跳频需双方配置一致；配对在基频进行。
- 半双工停等协议，吞吐约为 LoRa 物理速率的 50–70%（理论估算：SF7/BW125 约 300–450 B/s，未实测）。需要更高速率请提高 BW、降低 SF 或关闭 ACK。
- 配对时密钥以明文在空中交换（与多数同类设备相同）；对安全性有要求时请用 `AT+KEY` 在两端手动设置密钥。
