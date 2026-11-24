# LoRa Bridge

基于 fakeTec（nice!nano nRF52840 + Ra-01S + OLED）的 LoRa 透明串口桥：两块板配对后，把一端的 USB / 蓝牙串口数据通过 LoRa 传到另一端。

| 目录 | 内容 |
|---|---|
| [`firmware/`](firmware/) | nRF52840 固件（PlatformIO）：LoRa 跳频 / 重传 / 加密 / 压缩、2.4G 高速模式、ETH 网卡模式（USB CDC-NCM + DHCP）、省电待机、USB 串口 + WebUSB、蓝牙串口与配置、UF2 / DFU 升级 |
| [`client-rust/`](client-rust/) | 命令行客户端 `lbridge`：彩色终端 + 配置（Windows / Linux / macOS） |
| [`client-web/`](client-web/) | WebUSB 网页控制台：终端 + 配置 |
| [`openwrt/luci-app-lora-bridge/`](openwrt/luci-app-lora-bridge/) | OpenWrt 插件：设备插入时启动 Docker 容器并把容器内的虚拟串口连到设备 |

## 构建

推送 `v*` 标签时，GitHub Actions（`.github/workflows/build.yml`）构建全部产物并上传到该标签的 Release（也可以在 Actions 页面手动运行，只构建不发布）：

| 产物 | 说明 |
|---|---|
| `firmware` | `.uf2`（拖放刷机）、`.zip`（串口 DFU / Meshtastic 网页刷机器）、`.hex` |
| `lbridge-*` | Linux x86_64 / aarch64 / armv7（musl 静态）、Windows、macOS |
| `web-console.zip` | 静态站点；同时发布到 GitHub Pages（需在仓库设置里把 Pages 来源设为 GitHub Actions，未开启时该步骤失败但不影响 Release） |
| `openwrt-ipk` / `openwrt-apk` | `luci-app-lora-bridge`，架构无关（all），分别用 OpenWrt 24.10 和 25.12 SDK 打包 |

本地构建见各目录的 README。
## 许可证

[GNU Affero General Public License v3.0](LICENSE)（AGPL-3.0-only）。

`client-web/vendor/` 中的 xterm.js 为 MIT 许可，版权归其作者所有。
