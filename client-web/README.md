# LoRa Bridge 网页客户端（WebUSB / 蓝牙）

浏览器里的串口终端 + 配置界面，通过 WebUSB 或蓝牙（Web Bluetooth）连接设备（固件 1.1.0 及以上），不需要安装驱动或程序。

- 终端：到对端的透明串口，基于 xterm.js，支持完整 ANSI 颜色（16 / 256 / 真彩色）和控制序列，可直接操作远端 Linux 控制台（vi、htop、Ctrl-C、方向键等）。
- 配置：读取并修改全部设备参数，配对 / 解除配对、统计、重启、进入升级模式、任意 AT 命令。
- 状态栏：链路状态（LINK / STBY / NOOP / UNPR）、主从、信号、速率、电量，每 2 秒刷新。
- **2.4G 高速**按钮（固件 1.3.0+）：把数据切换到 2.4GHz 高速链路（`AT+FAST=1`），再点一次切回 LoRa；协商失败或设备自动回退到 LoRa 时弹窗说明原因。高速模式与蓝牙互斥，通过蓝牙连接本页面时该按钮不可用。

## 使用

需要 Chrome 或 Edge（桌面版或 Android）。页面必须通过 `http://localhost` 或 https 打开：

```
cd client-web
python -m http.server 8080
```

然后访问 <http://localhost:8080>，点"USB 连接"或"蓝牙连接"。也可以把整个目录放到任意 https 静态站点上。

- Windows：固件自带 WinUSB 描述符，系统自动装驱动。
- Linux：把 `99-lorabridge.rules` 复制到 `/etc/udev/rules.d/` 后重新插拔，否则浏览器没有权限打开设备。

### 蓝牙

点"蓝牙连接"，在列表里选择 `LoRaBridge-xxxx`。终端、配置和状态栏与 USB 方式完全相同，同一时间只使用一种连接。

- 默认不需要在系统里配对。新的电脑 / 手机第一次连接时设备屏幕会显示 `ALLOW?`，60 秒内**短按设备按钮**允许即可；页面会一直等到你按下（可点“取消”）。之后重连不用再按。需要固件 1.2.1 或更新。
- 设备设置为 `AT+BLEPAIR=2`（数字比对）或 `3`（加密配对）时，由操作系统完成配对，还要按系统提示确认。
- 页面自身的提示（连接进度、错误）用弹窗显示，终端里只出现对端发来的数据。
- 蓝牙串口接入同样会激活 LoRa 链路（对端也有串口在用时进入 LINK）。
- 需要浏览器支持 Web Bluetooth：Windows / macOS / Linux / Android 上的 Chrome 或 Edge。iOS 的浏览器不支持。
- 同一时间只进行一次连接尝试；若一直连不上，关闭占用该设备的其它标签页 / 程序，或在系统蓝牙设置里删除该设备的旧配对记录（设备端可用 `AT+BLEUNBOND`）后重试。

## 文件

| 文件 | 说明 |
|---|---|
| `index.html` `app.js` | 界面 |
| `bridge.js` | WebUSB / Web Bluetooth 传输和协议，浏览器与 Node 通用 |
| `vendor/` | xterm.js 5.5.0（离线可用） |
| `test/usb-test.js` | 用 node-usb 对真实硬件做无界面测试：`npm install && npm test` |

## 协议

WebUSB 接口（厂商类 0xFF，一对 bulk 端点）与 USB 串口等价，是到对端的透明数据通道。打开后需发一次类请求 `0x22`（value=1）告知固件有客户端接入。
蓝牙使用 Nordic UART Service（`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`）：向 RX（`…0002`）写入，从 TX（`…0003`）接收通知。
配置使用带内控制通道，任何时候可用，不需要 `+++`：

```
主机 -> 设备 : ESC ] L B ; <AT 命令> BEL
设备 -> 主机 : ESC ] L B ; <应答文本> BEL
```
