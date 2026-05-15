#!/bin/bash
# esdk-ctl.sh — 一键启停 ESDK 测试程序 (sample_cloud_api / dockerhelp)
#
# 用法:
#   ./esdk-ctl.sh start [sample|dock]   启动 (默认 sample)
#   ./esdk-ctl.sh stop                  停止所有 ESDK 进程
#   ./esdk-ctl.sh restart [sample|dock] 重启
#   ./esdk-ctl.sh status                查看进程 / 网卡 / 关键计数
#   ./esdk-ctl.sh log                   tail 日志
#   ./esdk-ctl.sh watch                 实时监视面板
#   ./esdk-ctl.sh setup-net             给网卡加 .55 副 IP (需要 sudo)

set -u

# ── 配置 ─────────────────────────────────────────────────────────────────
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SAMPLE_BIN="$REPO_ROOT/build/bin/sample_cloud_api"
DOCK_BIN="$REPO_ROOT/build/bin/dockerhelp"
SAMPLE_LOG="/tmp/sample_cloud_api2.log"
DOCK_LOG="/tmp/dockerhelp.log"
ESDK_LOCAL_IP="${ESDK_LOCAL_IP:-192.168.200.55}"
ESDK_REMOTE_IP="${ESDK_REMOTE_IP:-192.168.200.1}"
DOCK_SUBNET_PREFIX="${DOCK_SUBNET_PREFIX:-192.168.200.}"

# ── 颜色 ─────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
  R="\033[31m"; G="\033[32m"; Y="\033[33m"; B="\033[34m"; C="\033[36m"; D="\033[2m"; N="\033[0m"
else
  R=""; G=""; Y=""; B=""; C=""; D=""; N=""
fi
ok()   { echo -e "${G}✓${N} $*"; }
warn() { echo -e "${Y}⚠${N} $*"; }
err()  { echo -e "${R}✗${N} $*"; }
info() { echo -e "${B}ℹ${N} $*"; }

# ── 辅助 ─────────────────────────────────────────────────────────────────
running_pids() {
  pgrep -f "build/bin/sample_cloud_api|build/bin/dockerhelp" 2>/dev/null
}

count_in_log() {
  local log=$1 pat=$2
  [ -f "$log" ] || { echo 0; return; }
  local n
  n=$(grep -cE "$pat" "$log" 2>/dev/null | head -1)
  echo "${n:-0}"
}

write_ip_override() {
  echo "$ESDK_REMOTE_IP" > /tmp/test_remote_ip
  echo "$ESDK_LOCAL_IP"  > /tmp/test_local_ip
}

# ── 子命令 ───────────────────────────────────────────────────────────────

cmd_start() {
  local target=${1:-sample}
  local bin log name
  case "$target" in
    sample) bin=$SAMPLE_BIN; log=$SAMPLE_LOG; name=sample_cloud_api ;;
    dock)   bin=$DOCK_BIN;   log=$DOCK_LOG;   name=dockerhelp ;;
    *) err "未知目标: $target (期望 sample 或 dock)"; exit 1 ;;
  esac

  if [ ! -x "$bin" ]; then
    err "找不到可执行文件: $bin"
    info "先在仓库根目录执行: cmake --build build --target $name"
    exit 1
  fi

  # 检查是否已有同名进程
  local existing
  existing=$(pgrep -f "build/bin/$name")
  if [ -n "$existing" ]; then
    warn "$name 已在跑 (PID $existing); 用 'restart' 重启或 'stop' 停掉"
    exit 1
  fi

  # 网络前置: 检查 .55 副 IP 是否存在
  if ! ip -br addr | grep -q "$ESDK_LOCAL_IP"; then
    warn "网卡上没有 $ESDK_LOCAL_IP, ESDK 协议层握手会失败"
    info "运行 '$0 setup-net' 加副 IP, 或手动: sudo ip addr add $ESDK_LOCAL_IP/24 dev <网卡>"
  fi

  # 写 IP 覆盖文件 (dockerhelp 自己写, sample 不写)
  if [ "$target" = "sample" ]; then
    write_ip_override
    ok "IP 覆盖文件已写入 (local=$ESDK_LOCAL_IP, remote=$ESDK_REMOTE_IP)"
  fi

  # 备份旧日志
  [ -f "$log" ] && mv "$log" "${log}.bak"

  # 启动 (nohup 让它脱离当前 shell)
  cd "$REPO_ROOT"
  nohup "$bin" > "$log" 2>&1 &
  local pid=$!
  sleep 1
  if ! kill -0 "$pid" 2>/dev/null; then
    err "$name 启动后立刻退出, 查日志: $log"
    tail -10 "$log"
    exit 1
  fi
  ok "$name 已启动 (PID $pid)"
  info "日志: $log"
  info "看实时: $0 log"
  info "监视面板: $0 watch"
}

cmd_stop() {
  local pids
  pids=$(running_pids)
  if [ -z "$pids" ]; then
    info "没有 ESDK 进程在跑"
    return
  fi
  echo "$pids" | xargs -r kill 2>/dev/null
  sleep 1
  # 顽固的再来一次 SIGKILL
  local stuck
  stuck=$(running_pids)
  if [ -n "$stuck" ]; then
    echo "$stuck" | xargs -r kill -9 2>/dev/null
  fi
  ok "已停止 ESDK 进程"
}

cmd_restart() {
  cmd_stop
  sleep 1
  cmd_start "${1:-sample}"
}

cmd_status() {
  echo
  echo "═══ ESDK 程序状态 ═══"
  local pids
  pids=$(running_pids)
  if [ -z "$pids" ]; then
    err "没有进程在跑"
  else
    for p in $pids; do
      local cmd
      cmd=$(ps -p "$p" -o comm= 2>/dev/null)
      ok "PID $p — $cmd"
    done
  fi

  echo
  echo "═══ 网络 ═══"
  local nic_with_55
  nic_with_55=$(ip -br addr | awk -v ip="$ESDK_LOCAL_IP" '$0 ~ ip {print $1}' | head -1)
  if [ -n "$nic_with_55" ]; then
    ok "$ESDK_LOCAL_IP 在 $nic_with_55 上"
  else
    err "$ESDK_LOCAL_IP 没在任何网卡上 — ESDK 握手会失败"
  fi

  local dock_arp
  dock_arp=$(ip neigh | grep "$ESDK_REMOTE_IP " | head -1)
  if echo "$dock_arp" | grep -q "REACHABLE\|STALE\|DELAY"; then
    ok "Dock $ESDK_REMOTE_IP ARP: $(echo $dock_arp | awk '{print $NF}')"
  else
    err "Dock $ESDK_REMOTE_IP 无 ARP — 检查 USB 网卡 / 路由器"
  fi

  echo
  echo "═══ IP 覆盖文件 ═══"
  if [ -f /tmp/test_local_ip ] && [ -f /tmp/test_remote_ip ]; then
    ok "local=$(cat /tmp/test_local_ip)  remote=$(cat /tmp/test_remote_ip)"
  else
    warn "覆盖文件缺失 (启动 sample 时会自动写)"
  fi

  if [ -f "$SAMPLE_LOG" ]; then
    echo
    echo "═══ sample_cloud_api 日志计数 ═══"
    printf "  %-25s %s\n" "V1-Recv (协议接收)"        "$(count_in_log "$SAMPLE_LOG" 'V1-Recv')"
    printf "  %-25s %s\n" "Updating session key"      "$(count_in_log "$SAMPLE_LOG" 'Updating session key')"
    printf "  %-25s %s\n" "CloudMsg (云端推送)"       "$(count_in_log "$SAMPLE_LOG" 'cloud services|RegisterCustomServicesMessageHandler')"
    printf "  %-25s %s\n" "Activate / Bind / 鉴权OK"  "$(count_in_log "$SAMPLE_LOG" 'Application info verify successfully')"
    printf "  %-25s %s\n" "Error 行"                  "$(count_in_log "$SAMPLE_LOG" '\[Error\]')"
  fi
  echo
}

cmd_log() {
  local log
  if [ -f "$SAMPLE_LOG" ] && pgrep -f sample_cloud_api >/dev/null; then
    log=$SAMPLE_LOG
  elif [ -f "$DOCK_LOG" ] && pgrep -f dockerhelp >/dev/null; then
    log=$DOCK_LOG
  else
    err "没有活跃日志"; exit 1
  fi
  info "tail -f $log  (Ctrl+C 退出)"
  tail -f "$log"
}

cmd_watch() {
  if [ ! -f /tmp/watch-sample.sh ]; then
    cat > /tmp/watch-sample.sh <<'WATCH_EOF'
#!/bin/bash
LOG=/tmp/sample_cloud_api2.log
count() { local n; n=$(grep -cE "$1" "$LOG" 2>/dev/null | head -1); echo "${n:-0}"; }
echo "════════════════════════════════════════════════════════════════════════"
echo "  监视 sample_cloud_api  (Ctrl+C 退出监视, 不影响 sample 本身)"
echo "════════════════════════════════════════════════════════════════════════"
printf "  %-8s %-10s %-9s %-12s %-8s %-9s %s\n" 时间 进程 V1-Recv SessionKey 云消息 鉴权 新事件
echo "  ──────────────────────────────────────────────────────────────────────"
pr=0; ps=0; pc=0; pa=0
while true; do
  now=$(date '+%H:%M:%S')
  pid=$(pgrep -f "build/bin/sample_cloud_api" | head -1)
  if [ -z "$pid" ]; then printf "  %-8s ✗ 进程退出\n" "$now"; sleep 3; continue; fi
  r=$(count "V1-Recv"); s=$(count "Updating session key")
  c=$(count "cloud services|RegisterCustomServicesMessageHandler|CloudAPI_Recv")
  a=$(count "Application info verify successfully")
  f=""
  [ "$r" -gt "$pr" ] 2>/dev/null && f="$f recv+$((r-pr))"
  [ "$s" -gt "$ps" ] 2>/dev/null && f="$f skey+$((s-ps))"
  [ "$c" -gt "$pc" ] 2>/dev/null && f="$f ☁绑定信号!"
  printf "  %-8s PID%-7s %-9s %-12s %-8s %-9s %s\n" "$now" "$pid" "$r" "$s" "$c" "$a" "$f"
  pr=$r; ps=$s; pc=$c; pa=$a
  sleep 3
done
WATCH_EOF
    chmod +x /tmp/watch-sample.sh
  fi
  exec /tmp/watch-sample.sh
}

cmd_setup_net() {
  local nic
  nic=$(ip -br link | awk '/^enx|^eth|^enp/ && $2=="UP" {print $1}' | head -1)
  if [ -z "$nic" ]; then
    err "找不到 UP 状态的物理网卡 (enx*/eth*/enp*)"
    info "插上 USB 网卡或检查 'ip -br link'"
    exit 1
  fi
  info "目标网卡: $nic"
  if ip -br addr show "$nic" | grep -q "$ESDK_LOCAL_IP"; then
    ok "$ESDK_LOCAL_IP 已在 $nic 上, 无需操作"
    return
  fi
  info "需要 sudo 给 $nic 加副 IP $ESDK_LOCAL_IP/24"
  if sudo ip addr add "$ESDK_LOCAL_IP/24" dev "$nic"; then
    ok "副 IP 加好了"
    ip -br addr show "$nic"
  else
    err "sudo 失败"
    exit 1
  fi
}

# ── 入口 ─────────────────────────────────────────────────────────────────
case "${1:-}" in
  start)     shift; cmd_start "$@" ;;
  stop)      cmd_stop ;;
  restart)   shift; cmd_restart "$@" ;;
  status|s)  cmd_status ;;
  log|l)     cmd_log ;;
  watch|w)   cmd_watch ;;
  setup-net) cmd_setup_net ;;
  *)
    cat <<EOF
esdk-ctl.sh — ESDK 程序一键启停

用法:
  $0 start [sample|dock]   启动 (默认 sample_cloud_api)
  $0 stop                  停止所有 ESDK 进程
  $0 restart [sample|dock] 重启
  $0 status                查看进程 / 网卡 / 关键计数
  $0 log                   tail -f 日志
  $0 watch                 实时监视面板 (每 3s 刷新一行)
  $0 setup-net             给网卡加 .55 副 IP (需要 sudo)

环境变量:
  ESDK_LOCAL_IP   本机 IP (默认 192.168.200.55)
  ESDK_REMOTE_IP  Dock IP (默认 192.168.200.1)

示例:
  $0 status                看现在是什么情况
  $0 setup-net             第一次开机后配网
  $0 start sample          启动 sample_cloud_api 等绑定
  $0 watch                 看实时计数, 等 "☁绑定信号!" 出现
  $0 stop                  收工

EOF
    exit 1
    ;;
esac
