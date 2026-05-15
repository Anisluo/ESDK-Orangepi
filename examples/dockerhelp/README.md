# DockerHelp 调试速查

控制台调试 dockerhelp 的命令清单。所有命令在仓库根目录 `ESDK/` 下执行，除非另外注明。

> dockerhelp 是 ESDK 的一个调试 Web 面板：浏览器开 `http://<host>:8080` 看 SDK 状态、Dock OSD、媒体、云消息收发。
>
> Dock 默认期望网段 `192.168.200.x`。Dock 没接上时，HTTP 面板照常可用 —— SDK 初始化在后台线程，不会阻塞主线程。

---

## 1. 启停 dockerhelp

```bash
# 编译
cmake --build build --target dockerhelp -j$(nproc)

# 前台跑（Ctrl+C 退出）
./build/bin/dockerhelp

# 自定义端口
./build/bin/dockerhelp 9000

# 后台跑 + 日志到文件
./build/bin/dockerhelp > /tmp/dockerhelp.log 2>&1 &

# 查进程
pgrep -af dockerhelp

# 看实时日志
tail -f /tmp/dockerhelp.log

# 停掉
pkill -f dockerhelp
```

启动后控制台会打印：

```
==============================================
  DockerHelp 已启动 (Dock 期望网段: 192.168.200.x)
  浏览器访问: http://localhost:8080
==============================================
```

之后是 ESDK 库的鉴权 / 链路日志。Dock 还没接进来时会持续看到 `Command async send retry ... cmdSet=60 cmdId=64` 重试 —— 这是正常的，等握上手就消失。

---

## 2. 一键测试（推荐）

[test_dockerhelp.py](test_dockerhelp.py) 把所有 `/api/*` 端点走一遍，含 L3 连通性、字段校验、边界 case。零依赖（只用 Python stdlib，需 Python 3.10+）。

### 2.1 启动流程（两个终端）

测试脚本是 HTTP 客户端，**它本身不启动 dockerhelp** —— 你要先让 dockerhelp 跑起来，脚本再去打它。

**终端 A：启动被测服务**

```bash
cd ~/Desktop/ESDK                                    # 进仓库根目录
./build/bin/dockerhelp > /tmp/dockerhelp.log 2>&1 &  # 后台跑
sleep 2                                              # 等 HTTP server 起来
curl -s --noproxy '*' http://localhost:8080/api/status   # 确认能通
```

看到类似 `{"sdk_init_ok":false,"sdk_init_pending":...}` 的 JSON 输出就说明 dockerhelp OK。

**终端 B（或同一个终端）：跑测试**

```bash
cd ~/Desktop/ESDK
python3 examples/dockerhelp/test_dockerhelp.py 127.0.0.1:8080
```

预期输出：

```
DockerHelp 一键测试  目标: http://127.0.0.1:8080
  默认只读模式 (不改状态), 加 --with-mock 启用写入测试

[1/3] L3 连通性
  [PASS] ping 127.0.0.1
  [PASS] TCP 127.0.0.1:8080  可连接
  [PASS] HTTP / (主页)  HTTP 200, 15310 bytes
[2/3] /api/* 端点
  [PASS] /api/status  字段齐 (6 项)
  ...
══════════════════════════════════════════════
  16 PASS  0 FAIL  0 SKIP  耗时 1.3s
══════════════════════════════════════════════
```

**测完关掉服务**：

```bash
pkill -f dockerhelp
```

### 2.2 几种调用姿势

```bash
# ① 只读模式（默认；不会改 dockerhelp 状态，安全反复跑）
python3 examples/dockerhelp/test_dockerhelp.py 127.0.0.1:8080

# ② 远程测：脚本在 A 机，dockerhelp 跑在 B 机
python3 examples/dockerhelp/test_dockerhelp.py 192.168.200.55
python3 examples/dockerhelp/test_dockerhelp.py 192.168.200.55:8080

# ③ 写入测试：注入 mock Dock 数据、试发云消息、试边界 case
#    会改变 dockerhelp 的 Dock 面板显示，跑完面板会显示 battery=73 mode=test_probe
python3 examples/dockerhelp/test_dockerhelp.py 127.0.0.1:8080 --with-mock

# ④ 把详细结果存成 JSON（含每个 case 的 status/detail + API 快照）
python3 examples/dockerhelp/test_dockerhelp.py 127.0.0.1:8080 --json /tmp/report.json
cat /tmp/report.json | python3 -m json.tool | less

# ⑤ 帮助
python3 examples/dockerhelp/test_dockerhelp.py --help
```

退出码：全过 = 0，有 FAIL = 1。可直接用在 CI / Makefile / shell 脚本里串测试链。

### 2.3 测试脚本常见报错

| 报错 | 原因 | 处理 |
|---|---|---|
| `ConnectionRefusedError: [Errno 111]` | dockerhelp 没跑 / 端口不对 | 先 `pgrep -af dockerhelp` 确认进程，再 `ss -tlnp \| grep 8080` 确认端口 |
| 所有 case `[FAIL] ping ...` | 目标 IP 不可达 / 防火墙 | 手动 `ping <IP>`，检查防火墙 `sudo iptables -L` |
| `502 Bad Gateway` | shell 有 `http_proxy` 转发了请求 | `unset http_proxy https_proxy` 或临时 `env -u http_proxy python3 ...` |
| `SyntaxError` / `match` 关键字报错 | Python < 3.10 | 升级 Python，或 `python3.10 examples/dockerhelp/test_dockerhelp.py ...` |

---

## 3. 用 curl 手工打接口

> 如果系统设了 `http_proxy`，记得加 `--noproxy '*'`，否则代理会拦截 `127.0.0.1` 的请求并回 502。

### 3.1 SDK & 总览

```bash
# SDK 状态（init 是否完成、错误码、媒体数、云消息数）
curl -s --noproxy '*' http://localhost:8080/api/status | python3 -m json.tool

# 实时日志（滚动 200 行）
curl -s --noproxy '*' http://localhost:8080/api/logs | python3 -m json.tool

# 清空日志
curl -s --noproxy '*' -X POST http://localhost:8080/api/logs/clear
```

### 3.2 Dock OSD

```bash
# 当前 Dock 状态（valid=false 说明云端还没透传数据过来）
curl -s --noproxy '*' http://localhost:8080/api/dock | python3 -m json.tool

# 注入一组示例数据预览面板（Dock 没接时也能用）
curl -s --noproxy '*' -X POST http://localhost:8080/api/dock/inject

# 注入自定义字段
curl -s --noproxy '*' -X POST \
  -H 'Content-Type: application/json' \
  -d '{"type":"dock_osd","battery":42,"temperature":-3.5,"mode":"working","wind_speed":12.7,"rainfall":3,"cover":1}' \
  http://localhost:8080/api/dock/inject

curl -s --noproxy '*' http://localhost:8080/api/dock | python3 -m json.tool
```

### 3.3 云消息

```bash
# 看 Dock 透传过来的云消息（滚动 50 条）
curl -s --noproxy '*' http://localhost:8080/api/cloud_messages | python3 -m json.tool

# 主动发一条云消息（最多 256 字节，SDK 未初始化时返回 ok=false）
curl -s --noproxy '*' -X POST \
  -H 'Content-Type: application/json' \
  -d '{"message":"hello from console"}' \
  http://localhost:8080/api/send
```

### 3.4 媒体文件

```bash
# 拉取飞机媒体文件列表（首次会触发 SDK 拉文件，稍慢）
curl -s --noproxy '*' http://localhost:8080/api/media | python3 -m json.tool
```

### 3.5 网络诊断

```bash
# 检查本机网卡是否在 192.168.200.x，并 ping 192.168.200.100
curl -s --noproxy '*' http://localhost:8080/api/net | python3 -m json.tool
```

返回 `in_dock_subnet=false` 或 `dock_pingable=false` —— 检查物理网线 + 静态 IP 配置（见下面"快速排错"）。

---

## 4. ESDK 初始化五关诊断

ESDK 从 `ESDKInit()` 调用到能拿到 Dock 状态，要过 5 个关卡。卡在哪一关，处理方式完全不同。`/api/status` 的 `sdk_init_stage` 字段会告诉你当前在哪。

| stage | 含义 | 通过标志 | 卡住时的处理 |
|---|---|---|---|
| `authenticating` | 验证 App ID/Key/License | 日志：`Application info verify successfully` | 检查 [examples/init/app_info.h](../init/app_info.h) 凭证 |
| `handshaking` | 与 Dock 走 PSDK 私有协议握手 | 日志出现 `V1-Recv: 0xA6->0xF6` | 检查 IP 覆盖（见下面 4.1），检查 Dock 是否上电 |
| `awaiting_pilot_binding` | 等 Pilot 2 给 App 预绑定授权 | 日志：`Updating session key...` 停止刷屏 | 用 DJI Pilot 2 绑定（见 4.2） |
| `ready` | SDK 完全就位 | `sdk_init_ok=true` | 可以收 `/api/cloud_messages`、拉 `/api/media` |
| `failed` | `ESDKInit` 返回非 0 | `sdk_init_error` 非 0 | 看 `sdk_init_error` 错误码 |

### 4.1 IP 覆盖机制（绕开 ESDK 默认 .55/.100）

ESDK 默认假设："PSDK 物理网口直连，本机 .55，对端 .100"。**但如果你的 Dock 在普通 LAN 里**（比如通过路由器、USB 网卡），真实 IP 通常不是 .100。

DJI 在 libedgesdk.a 里留了一个**文件覆盖通道**：

```
/tmp/test_remote_ip   ← Dock 真实 IP (默认 192.168.200.1)
/tmp/test_local_ip    ← 本机 IP (默认 192.168.200.55)
```

dockerhelp **启动时自动写这两个文件**，所以一般不用管。要自定义，启动前设环境变量：

```bash
DOCK_REMOTE_IP=192.168.200.1 \
DOCK_LOCAL_IP=192.168.200.55 \
./build/bin/dockerhelp
```

**关键约束**：`DOCK_LOCAL_IP` 必须真的在你机器上（用 `ip addr add` 加副地址）；Dock 仍然只接受源 IP = `192.168.200.55`，**不能用 DHCP 拿到的随机 IP**（Dock 协议层白名单）。

加副 IP 的命令：

```bash
# 假设你的 USB 网卡叫 enxXXXX
sudo ip addr add 192.168.200.55/24 dev enxXXXX
```

### 4.2 在 DJI Pilot 2 里预绑定 App

ESDK 协议握手成功后，**Dock 仍然不会发任何数据**给你 —— 它要求云端在 Pilot 2 里**预绑定**你这个 App。

操作：

1. 用注册了你 App ID 的**开发者账号**登录 DJI Pilot 2
2. Pilot 2 连上这台 Dock
3. **机场设置 → Edge SDK / 第三方载荷管理**
4. 选中你的 App（名字看 `app_info.h` 里的 `USER_APP_NAME`）→ **绑定 / 启用**

绑完后**不用重启 Dock**，但 dockerhelp 需要重启让 ESDK 重新协商 session key。

绑定卡住时，dockerhelp Web 面板会自动弹出黄色提示卡告诉你怎么做（基于 `sdk_init_stage = awaiting_pilot_binding` 自动判断）。

### 4.3 关于 "ping 不通 Dock"

**这是预期行为，不是 bug**。DJI Dock 不响应 ICMP，只响应自家 PSDK 协议。判断"链路通不通"的真正标志：

- ❌ ping 192.168.200.1：永远 100% 丢包，**正常**
- ✅ `ip neigh | grep 200.1`：能看到 Dock 的 MAC 地址 = L2 通
- ✅ ESDK 日志出现 `V1-Recv: 0xA6->0xF6` = 协议层通

---

## 5. 快速排错

> 看 §4 的"初始化五关"判断 ESDK 卡在哪。这一节是其他通用问题。

| 现象 | 大概率原因 | 处理 |
|---|---|---|
| 浏览器 / curl 卡住 502 | shell 有 `http_proxy` | 加 `--noproxy '*'`，或 `unset http_proxy https_proxy` |
| 持续 `cmdSet=60 retry`，**无 V1-Recv** | 协议握手未完成 | 检查 IP 覆盖（§4.1）、Dock 是否上电、ARP `ip neigh \| grep 200.1` 是否有 MAC |
| 持续 `Updating session key...` | 卡在 §4.2 预绑定 | 去 Pilot 2 绑 App，重启 dockerhelp |
| `Edge sdk init failed: 11` | `app_info.h` 是占位符或凭证不对 | 填回真实 `APP_ID/KEY/LICENSE` 重编译 |
| `/api/dock` 一直 `valid=false` | 云端没下发 `{"type":"dock_osd",...}` | 用 `/api/dock/inject` 先验证面板，再配置云端透传 |
| 进程 exit 139 (segfault) | 撞到了已知边界（理论上已修） | 用 `dmesg | tail` 看 core，参考 [test_dockerhelp.py](test_dockerhelp.py) 的边界 case 复现 |

### 切换 dockerhelp 的网卡 IP（虚拟机 / Orange Pi 上）

```bash
# 看当前网卡和 IP
ip -br addr

# 给 Dock 网段加副地址 (保留 DHCP 的主 IP)
# 把 enxXXXX 换成你 USB/物理网卡名
sudo ip addr add 192.168.200.55/24 dev enxXXXX
```

> ping Dock 永远不通 —— 看 §4.3 解释，判断链路用 ESDK 日志或 `ip neigh`。

---

## 6. 端点速查表

| 方法 | 路径 | 说明 |
|---|---|---|
| GET  | `/`                     | Web 面板 HTML |
| GET  | `/api/status`           | SDK 初始化 / 连接状态。含 `sdk_init_stage`: `authenticating` / `handshaking` / `awaiting_pilot_binding` / `ready` / `failed`，及 `sdk_init_elapsed_ms` |
| GET  | `/api/logs`             | 实时日志（滚动 200） |
| POST | `/api/logs/clear`       | 清空日志 |
| GET  | `/api/cloud_messages`   | 收到的云消息（滚动 50） |
| POST | `/api/send`             | 发送云消息到 Dock，body `{"message":"..."}` ≤256B |
| GET  | `/api/media`            | 飞机媒体文件列表（触发 SDK 拉取） |
| GET  | `/api/dock`             | 当前 Dock OSD 数据 |
| POST | `/api/dock/inject`      | 注入测试 OSD 数据，body 为 dock_osd JSON 或空（用默认） |
| GET  | `/api/net`              | 本机网卡 + ping Dock 结果。可用 `DOCK_IP` 环境变量覆盖目标 IP |

### Dock OSD 透传 JSON 约定

云端要把 Dock 的 OSD 透传到 dockerhelp，请按这个 schema 包装并通过 ESDK `custom_data_transmission_to_esdk` 下发（≤256 字节）：

```json
{"type":"dock_osd",
 "dock_sn":"...","aircraft_sn":"...",
 "battery":85,"temperature":24.5,"humidity":60,
 "wind_speed":3.4,"rainfall":0,
 "lon":113.93,"lat":22.53,"alt":12.3,
 "mode":"idle","cover":0,"signal_4g":4}
```

字段缺失时保留上次值，多余字段忽略。
