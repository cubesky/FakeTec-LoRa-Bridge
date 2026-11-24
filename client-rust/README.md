# lbridge — LoRa Bridge 命令行客户端（Rust）

串口终端 + 配置工具，支持 Windows / Linux / macOS，通过 USB 串口连接设备（固件 1.1.0 及以上）。

## 编译

```
cargo build --release        # 生成 target/release/lbridge
```

Linux 下不依赖 libudev。无权限打开 `/dev/ttyACM*` 时，把 `../client-web/99-lorabridge.rules` 装到 `/etc/udev/rules.d/`。

## 终端

```
lbridge                 # 只插了一个设备时自动选择
lbridge -p /dev/ttyACM0 term
lbridge -p COM3 term --echo
```

本地控制台进入 raw 模式，所有字节原样透传：

- 远端的 ANSI 颜色（16 / 256 / 真彩色）、光标控制、全屏程序（vi、htop、tmux）正常显示；
- Ctrl-C、Ctrl-Z、方向键、功能键等控制键和转义序列原样发往远端（Windows 下自动启用 VT 输入 / 输出）；
- 窗口标题每 2 秒显示链路状态、信号和速率（`--no-title` 关闭）。

`Ctrl-]` 打开本地菜单：`q` 退出，`s` 状态，`i` 设备信息，`e` 全部设置，`p` 配对，`f` 切换 2.4G 高速模式（FAST，开 / 关），`c` 输入一条命令（如 `freq 434.5` 或 `AT+SF=9`），再按一次 `Ctrl-]` 把该键发给远端。高速模式启用时窗口标题和状态行显示 `FAST 2.4G`。

## 配置

```
lbridge list                 # 列出已插入的设备
lbridge info                 # 设备信息
lbridge stat                 # 链路状态和统计
lbridge cfg                  # 全部设置
lbridge get freq
lbridge set freq 434.5
lbridge at AT+HOPCH=16       # 任意 AT 命令
lbridge pair                 # 两端各执行一次
lbridge unpair | reboot | dfu
lbridge monitor              # 实时状态行
lbridge fast on              # 协商切换到 2.4GHz 高速模式（固件 1.3.0+），等待结果；失败时给出原因
lbridge fast off             # 切回 LoRa
lbridge fast                 # 查询高速模式状态
lbridge eth on               # 开启网卡模式（固件 1.4.0+）：两端各多出一个 USB 网卡并自动分配 IP；USB 会重新枚举
lbridge eth off | eth        # 关闭 / 查询状态
lbridge set ethcfg 10.211.0.0/30,10.211.0.1,10.211.0.2   # 网段、主端 IP、从端 IP（也可分别 set ethnet / ethmip / ethsip）
```

配置命令使用设备的带内控制通道（`ESC ] L B ; <AT 命令> BEL`），终端运行期间也可以用，不会混入串口数据，也不需要 `+++`。