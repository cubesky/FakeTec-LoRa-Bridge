# LoRa Bridge 网页客户端（WebUSB）

浏览器里的串口终端 + 配置界面，直接通过 WebUSB 连接设备（固件 1.1.0 及以上），不需要安装驱动或程序。

- 终端：到对端的透明串口，基于 xterm.js，支持完整 ANSI 颜色（16 / 256 / 真彩色）和控制序列，可直接操作远端 Linux 控制台（vi、htop、Ctrl-C、方向键等）。
- 配置：读取并修改全部设备参数，配对 / 解除配对、统计、重启、进入升级模式、任意 AT 命令。
- 状态栏：链路状态（LINK / STBY / NOOP / UNPR）、主从、信号、速率、电量，每 2 秒刷新。

## 使用

需要 Chrome 或 Edge（桌面版或 Android）。页面必须通过 `http://localhost` 或 https 打开：

```
cd client-web
python -m http.server 8080
```

然后访问 <http://localhost:8080>，点"连接设备"。也可以把整个目录放到任意 https 静态站点上。

- Windows：固件自带 WinUSB 描述符，系统自动装驱动。
- Linux：把 `99-lorabridge.rules` 复制到 `/etc/udev/rules.d/` 后重新插拔，否则浏览器没有权限打开设备。

## 文件

| 文件 | 说明 |
|---|---|
| `index.html` `app.js` | 界面 |
| `bridge.js` | WebUSB 传输和协议，浏览器与 Node 通用 |
| `vendor/` | xterm.js 5.5.0（离线可用） |
| `test/usb-test.js` | 用 node-usb 对真实硬件做无界面测试：`npm install && npm test` |

## 协议

WebUSB 接口（厂商类 0xFF，一对 bulk 端点）与 USB 串口等价，是到对端的透明数据通道。打开后需发一次类请求 `0x22`（value=1）告知固件有客户端接入。
配置使用带内控制通道，任何时候可用，不需要 `+++`：

```
主机 -> 设备 : ESC ] L B ; <AT 命令> BEL
设备 -> 主机 : ESC ] L B ; <应答文本> BEL
```