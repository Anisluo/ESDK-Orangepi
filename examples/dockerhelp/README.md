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

## 4. 快速排错

| 现象 | 大概率原因 | 处理 |
|---|---|---|
| 浏览器 / curl 卡住 502 | shell 有 `http_proxy` | 加 `--noproxy '*'`，或 `unset http_proxy https_proxy` |
| 控制台一直 `Command async send retry, cmdSet=60` | Dock 没接 / 网段不对 | 看 `/api/net`：`in_dock_subnet` 必须 `true`，`dock_pingable` 必须 `true` |
| `Edge sdk init failed: 11` | `app_info.h` 是占位符或凭证不对 | 填回真实 `APP_ID/KEY/LICENSE` 重编译 |
| `/api/dock` 一直 `valid=false` | 云端没下发 `{"type":"dock_osd",...}` | 用 `/api/dock/inject` 先验证面板，再配置云端透传 |
| 进程 exit 139 (segfault) | 撞到了已知边界（理论上已修） | 用 `dmesg | tail` 看 core，参考 [test_dockerhelp.py](test_dockerhelp.py) 的边界 case 复现 |

### 切换 dockerhelp 的网卡 IP（虚拟机 / Orange Pi 上）

```bash
# 看当前网卡和 IP
ip -br addr

# 临时切到 Dock 网段（重启失效）
sudo ip addr flush dev ens33
sudo ip addr add 192.168.200.55/24 dev ens33
sudo ip link set ens33 up

# 验证能不能 ping 通 Dock
ping -c 3 192.168.200.100
```

---

## 5. 端点速查表

| 方法 | 路径 | 说明 |
|---|---|---|
| GET  | `/`                     | Web 面板 HTML |
| GET  | `/api/status`           | SDK 初始化 / 连接状态 |
| GET  | `/api/logs`             | 实时日志（滚动 200） |
| POST | `/api/logs/clear`       | 清空日志 |
| GET  | `/api/cloud_messages`   | 收到的云消息（滚动 50） |
| POST | `/api/send`             | 发送云消息到 Dock，body `{"message":"..."}` ≤256B |
| GET  | `/api/media`            | 飞机媒体文件列表（触发 SDK 拉取） |
| GET  | `/api/dock`             | 当前 Dock OSD 数据 |
| POST | `/api/dock/inject`      | 注入测试 OSD 数据，body 为 dock_osd JSON 或空（用默认） |
| GET  | `/api/net`              | 本机网卡 + ping Dock 结果 |

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
