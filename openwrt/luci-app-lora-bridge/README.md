# luci-app-lora-bridge

OpenWrt 插件：检测到 LoRa Bridge（USB `239a:00b3`）插入时启动（或创建）指定的 Docker 容器，并把容器内的虚拟串口连接到该设备；拔出一段时间后自动停止容器，在这段时间内重新插入只会重新连接。

只包含 shell 脚本和 LuCI JavaScript，包架构为 `all`。依赖 `socat`、`kmod-usb-acm`，以及系统里可用的 `docker` 命令（`dockerd` + `docker`，未列为硬依赖）。

## 工作方式

- 服务 `lora-bridged` 每 2 秒检查一次，并由 tty 热插拔事件立即唤醒。
- 插入：按配置创建或启动容器，然后把串口接进去。
- 拔出：断开串口并开始计时（`stop_delay`，默认 300 秒），到时停止容器。计时内重新插入则取消计时并重新连接，容器不重启。
- 容器内的串口是 `<mount_path>/ttyLORA`（默认 `/dev/lora/ttyLORA`），路径同时通过环境变量 `LORA_BRIDGE_PORT` 传入。

两种串口模式：

| 模式 | 说明 |
|---|---|
| `pty`（默认） | 虚拟串口。由一个常驻的 socat 持有伪终端，设备在时另一个 socat 负责转发。容器里的程序在拔插期间可以一直开着这个端口。 |
| `device` | 把真实 tty 的设备节点放进容器。拔插后程序需要重新打开。 |

串口通过宿主机目录 `/var/run/lora-bridge/dev`（独立的共享 tmpfs）进入容器。由本插件创建的容器会自动带上
`-v /var/run/lora-bridge/dev:<mount_path>:rslave` 和所需的设备 cgroup 规则；使用已有容器时需要自己加上这些参数，否则状态页会给出警告。

## 界面

服务 → LoRa Bridge：设备与连接状态、容器状态、自动停止倒计时、启动 / 停止 / 重建容器，以及全部设置（USB ID、串口模式、停止延时、容器名、镜像、命令、网络、环境变量、卷、端口、额外参数等）。

## 命令行

```
lora-bridge status           # JSON
lora-bridge status --text
lora-bridge start | stop | recreate
lora-bridge logs [n]
```

`status` 输出示例：

```json
{"service":{"enabled":true,"running":true,"pid":1234},
 "device":{"present":true,"path":"/dev/ttyACM0","vid":"239a","pid":"00b3","serial":"4A9156FC44CFA70F","product":"FakeTec LoRa Bridge"},
 "link":{"mode":"pty","attached":true,"baud":115200,"container_port":"/dev/lora/ttyLORA","host_port":"/var/run/lora-bridge/dev/ttyLORA","connected_since":1700000000,"connected_seconds":42},
 "container":{"name":"lora-bridge","exists":true,"state":"running","id":"abcdef012345","image":"alpine:latest","started_at":"...","port_mounted":true,"managed":true},
 "timer":{"stop_delay":300,"unplugged_at":null,"stop_in":null},
 "error":""}
```

拔出后的计时期间 `timer.stop_in` 为剩余秒数。

## 构建

仓库的 GitHub Actions 会用 OpenWrt SDK 生成 `.ipk`（24.10）和 `.apk`（25.12）。手动构建：把本目录放进 SDK 的某个 feed 后 `make package/luci-app-lora-bridge/compile`。

脚本需要可执行位。从 Windows 提交时请执行一次：

```
git update-index --chmod=+x openwrt/luci-app-lora-bridge/root/etc/init.d/lora-bridge openwrt/luci-app-lora-bridge/root/usr/sbin/lora-bridged openwrt/luci-app-lora-bridge/root/usr/bin/lora-bridge
```

（CI 和安装后的 uci-defaults 脚本也会补上可执行位。）

## 测试情况

守护进程和 CLI 的逻辑在 Linux 上用模拟的 docker 和模拟串口设备跑通过（插入、连接、宽限期内重插、超时停止、JSON 输出）。
尚未在真实 OpenWrt + Docker 上验证的部分：SDK 打包、LuCI 页面、共享挂载把虚拟串口传进容器（依赖 busybox `mount --make-shared` 和 Docker 的 `rslave` 挂载）。