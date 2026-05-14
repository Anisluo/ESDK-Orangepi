/**
 * DJI Dock2 Debug Assistant - DockerHelp
 * Web-based debug dashboard for DJI Edge SDK
 * Access via browser: http://localhost:8080
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cloud_api.h"
#include "logger.h"
#include "media_manager.h"

#include "../../third_party/httplib.h"

using namespace edge_sdk;

ErrorCode ESDKInit();

// ─── Global State ────────────────────────────────────────────────────────────

struct DockStatus {
    bool        valid       = false;
    bool        from_mock   = false;
    std::string updated_at;          // "HH:MM:SS"
    std::string dock_sn;
    std::string aircraft_sn;
    int         battery_pct = -1;    // 0..100
    double      temperature = 0.0;   // ℃
    int         humidity    = -1;    // 0..100
    double      wind_speed  = 0.0;   // m/s
    int         rainfall    = -1;    // 0=none 1=light 2=mid 3=heavy
    double      longitude   = 0.0;
    double      latitude    = 0.0;
    double      altitude    = 0.0;
    std::string mode;                // "idle" / "working" / "charging" ...
    int         cover_state = -1;    // 0=closed 1=open 2=half
    int         signal_4g   = -1;    // 0..5 bars
};

struct AppState {
    std::atomic<bool>        sdk_connected{false};
    std::atomic<bool>        sdk_init_ok{false};
    std::atomic<bool>        sdk_init_pending{true};
    std::atomic<int>         sdk_init_error{0};
    std::string              last_cloud_msg;
    std::mutex               log_mutex;
    std::deque<std::string>  log_lines;       // rolling 200 lines
    std::deque<std::string>  cloud_messages;  // rolling 50 messages
    std::mutex               cloud_mutex;

    std::vector<std::string> media_file_names;
    std::mutex               media_mutex;

    DockStatus               dock;
    std::mutex               dock_mutex;

    void pushLog(const std::string& line) {
        std::lock_guard<std::mutex> lk(log_mutex);
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[20];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
        log_lines.push_back(std::string(buf) + "  " + line);
        if (log_lines.size() > 200) log_lines.pop_front();
    }

    void pushCloud(const std::string& msg) {
        std::lock_guard<std::mutex> lk(cloud_mutex);
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[20];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
        cloud_messages.push_back(std::string(buf) + "  " + msg);
        if (cloud_messages.size() > 50) cloud_messages.pop_front();
        last_cloud_msg = msg;
    }
} g_state;

// ─── ESDK Callbacks ──────────────────────────────────────────────────────────

static void applyDockJson(const std::string& payload);  // defined below

void OnCloudMessage(const uint8_t* data, uint32_t len) {
    std::string msg(reinterpret_cast<const char*>(data), len);
    g_state.pushLog("[Cloud←Dock] " + msg);
    g_state.pushCloud(msg);
    applyDockJson(msg);
}

ErrorCode OnNewMediaFile(const MediaFile& f) {
    std::string info = f.file_name + "  (" +
                       std::to_string(f.file_size / 1024) + " KB)";
    g_state.pushLog("[Media] new file: " + info);
    std::lock_guard<std::mutex> lk(g_state.media_mutex);
    g_state.media_file_names.push_back(info);
    return kOk;
}

// ─── Minimal JSON field extraction (no external deps) ────────────────────────
// Cloud payload contract (you decide what your cloud-side bridge sends):
//   {"type":"dock_osd",
//    "dock_sn":"...","aircraft_sn":"...",
//    "battery":85,"temperature":24.5,"humidity":60,
//    "wind_speed":3.4,"rainfall":0,
//    "lon":113.93,"lat":22.53,"alt":12.3,
//    "mode":"idle","cover":0,"signal_4g":4}
// Unknown / missing fields stay at their default values.

static bool jsonFindKey(const std::string& s, const std::string& key,
                        size_t* value_start) {
    std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle);
    if (p == std::string::npos) return false;
    p = s.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    *value_start = p;
    return p < s.size();
}

static bool jsonGetString(const std::string& s, const std::string& key,
                          std::string* out) {
    size_t p;
    if (!jsonFindKey(s, key, &p)) return false;
    if (s[p] != '"') return false;
    size_t end = p + 1;
    while (end < s.size() && s[end] != '"') {
        if (s[end] == '\\' && end + 1 < s.size()) end += 2;
        else end++;
    }
    if (end >= s.size()) return false;
    *out = s.substr(p + 1, end - p - 1);
    return true;
}

static bool jsonGetNumber(const std::string& s, const std::string& key,
                          double* out) {
    size_t p;
    if (!jsonFindKey(s, key, &p)) return false;
    if (s[p] == '"') return false;  // we want numbers, not strings
    char* endp = nullptr;
    double v = std::strtod(s.c_str() + p, &endp);
    if (endp == s.c_str() + p) return false;
    *out = v;
    return true;
}

static void applyDockJson(const std::string& payload) {
    std::string s;  // probe a "type" field to confirm this is a status payload
    if (!jsonGetString(payload, "type", &s)) return;
    if (s != "dock_osd" && s != "dock_status") return;

    std::lock_guard<std::mutex> lk(g_state.dock_mutex);
    DockStatus& d = g_state.dock;
    d.valid = true;
    d.from_mock = false;

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    d.updated_at = buf;

    std::string sv;
    if (jsonGetString(payload, "dock_sn", &sv))     d.dock_sn = sv;
    if (jsonGetString(payload, "aircraft_sn", &sv)) d.aircraft_sn = sv;
    if (jsonGetString(payload, "mode", &sv))        d.mode = sv;

    double nv;
    if (jsonGetNumber(payload, "battery", &nv))     d.battery_pct = (int)nv;
    if (jsonGetNumber(payload, "temperature", &nv)) d.temperature = nv;
    if (jsonGetNumber(payload, "humidity", &nv))    d.humidity = (int)nv;
    if (jsonGetNumber(payload, "wind_speed", &nv))  d.wind_speed = nv;
    if (jsonGetNumber(payload, "rainfall", &nv))    d.rainfall = (int)nv;
    if (jsonGetNumber(payload, "lon", &nv))         d.longitude = nv;
    if (jsonGetNumber(payload, "lat", &nv))         d.latitude = nv;
    if (jsonGetNumber(payload, "alt", &nv))         d.altitude = nv;
    if (jsonGetNumber(payload, "cover", &nv))       d.cover_state = (int)nv;
    if (jsonGetNumber(payload, "signal_4g", &nv))   d.signal_4g = (int)nv;
}

// ─── Network diagnostics (look for 192.168.200.x iface and ping Dock) ────────

struct NetInfo {
    std::string iface;
    std::string ip;
    bool        in_dock_subnet = false;
    bool        dock_pingable  = false;   // ping 192.168.200.100 once
};

static NetInfo collectNetInfo() {
    NetInfo info;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        std::string fallback_iface, fallback_ip;
        for (ifaddrs* p = ifs; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            std::string name = p->ifa_name ? p->ifa_name : "";
            std::string ip   = buf;
            if (name == "lo") continue;
            if (ip.rfind("192.168.200.", 0) == 0) {
                info.iface = name;
                info.ip = ip;
                info.in_dock_subnet = true;
                break;
            }
            if (fallback_iface.empty()) { fallback_iface = name; fallback_ip = ip; }
        }
        if (info.iface.empty()) { info.iface = fallback_iface; info.ip = fallback_ip; }
        freeifaddrs(ifs);
    }
    // single-shot ping; 1s timeout
    int rc = std::system("ping -c 1 -W 1 192.168.200.100 >/dev/null 2>&1");
    info.dock_pingable = (rc == 0);
    return info;
}

// ─── HTML Page (single-page, self-contained) ─────────────────────────────────

static const char* HTML = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>DockerHelp - DJI Dock2 调试助手</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:"PingFang SC","Segoe UI",sans-serif;background:#0d1117;color:#e6edf3;min-height:100vh}
header{background:#161b22;border-bottom:1px solid #30363d;padding:14px 24px;display:flex;align-items:center;gap:16px}
header h1{font-size:18px;font-weight:600;letter-spacing:.5px}
.badge{padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600}
.badge.ok{background:#1a4a2e;color:#3fb950}
.badge.err{background:#4a1a1a;color:#f85149}
.badge.warn{background:#4a3a1a;color:#d29922}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;padding:20px;max-width:1400px;margin:0 auto}
@media(max-width:900px){.grid{grid-template-columns:1fr}}
.card{background:#161b22;border:1px solid #30363d;border-radius:10px;overflow:hidden}
.card-header{padding:12px 16px;background:#1c2128;border-bottom:1px solid #30363d;display:flex;align-items:center;justify-content:space-between}
.card-header h2{font-size:14px;font-weight:600;color:#8b949e;text-transform:uppercase;letter-spacing:.8px}
.card-body{padding:14px}
.log-box{background:#0d1117;border:1px solid #21262d;border-radius:6px;height:220px;overflow-y:auto;padding:10px;font-family:"JetBrains Mono","Fira Code",monospace;font-size:12px;line-height:1.7;color:#8b949e}
.log-box span{display:block}
.log-box span.info{color:#79c0ff}
.log-box span.warn{color:#d29922}
.log-box span.err {color:#f85149}
.stat-row{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:12px}
.stat{flex:1;min-width:100px;background:#0d1117;border:1px solid #21262d;border-radius:8px;padding:12px;text-align:center}
.stat .val{font-size:26px;font-weight:700;color:#58a6ff}
.stat .lbl{font-size:11px;color:#8b949e;margin-top:4px}
.send-row{display:flex;gap:8px;margin-top:10px}
.send-row input{flex:1;background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:8px 12px;color:#e6edf3;font-size:13px;outline:none}
.send-row input:focus{border-color:#58a6ff}
.btn{padding:8px 16px;border-radius:6px;border:none;cursor:pointer;font-size:13px;font-weight:600;transition:.15s}
.btn-primary{background:#1f6feb;color:#fff}
.btn-primary:hover{background:#388bfd}
.btn-primary:disabled{background:#21262d;color:#484f58;cursor:not-allowed}
.btn-danger{background:#da3633;color:#fff}
.btn-danger:hover{background:#f85149}
.btn-sm{padding:5px 12px;font-size:12px}
.file-list{max-height:180px;overflow-y:auto}
.file-item{padding:6px 10px;border-bottom:1px solid #21262d;font-size:12px;color:#8b949e;display:flex;justify-content:space-between}
.file-item:hover{background:#1c2128}
.tag{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px;margin-right:4px}
.tag-jpeg{background:#1a3a4a;color:#79c0ff}
.tag-mp4{background:#2a1a4a;color:#d2a8ff}
.pulse{animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.divider{height:1px;background:#21262d;margin:10px 0}
#toast{position:fixed;bottom:24px;right:24px;background:#1f6feb;color:#fff;padding:10px 20px;border-radius:8px;font-size:13px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:999}
</style>
</head>
<body>
<header>
  <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#58a6ff" stroke-width="2"><path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/></svg>
  <h1>DockerHelp &nbsp;·&nbsp; DJI Dock2 调试助手</h1>
  <span id="sdk-badge" class="badge warn pulse">连接中...</span>
</header>

<div class="grid">
  <!-- 状态卡 -->
  <div class="card">
    <div class="card-header"><h2>SDK 状态</h2><button class="btn btn-primary btn-sm" onclick="refreshAll()">刷新</button></div>
    <div class="card-body">
      <div class="stat-row">
        <div class="stat"><div class="val" id="stat-conn">—</div><div class="lbl">连接状态</div></div>
        <div class="stat"><div class="val" id="stat-files">—</div><div class="lbl">媒体文件数</div></div>
        <div class="stat"><div class="val" id="stat-msgs">—</div><div class="lbl">云消息数</div></div>
      </div>
      <div class="divider"></div>
      <div style="font-size:12px;color:#8b949e;line-height:2">
        <div>App名称：<span style="color:#e6edf3">DockerAssistant</span></div>
        <div>App ID：<span style="color:#e6edf3">181760</span></div>
        <div>平台：<span style="color:#e6edf3">DJI Dock2 Edge SDK v1.2</span></div>
      </div>
    </div>
  </div>

  <!-- Dock 状态 -->
  <div class="card" style="grid-column:1/-1">
    <div class="card-header">
      <h2>机场状态 (Dock OSD)</h2>
      <div style="display:flex;gap:8px;align-items:center">
        <span id="dock-updated" style="font-size:11px;color:#8b949e">—</span>
        <button class="btn btn-primary btn-sm" onclick="injectMock()">注入示例数据</button>
      </div>
    </div>
    <div class="card-body">
      <div id="dock-empty" style="padding:14px;text-align:center;color:#484f58;font-size:13px">
        暂无数据。等待云端通过 ESDK 自定义透传下发 {"type":"dock_osd",...} 消息，
        或点击「注入示例数据」预览界面。
      </div>
      <div id="dock-grid" class="stat-row" style="display:none">
        <div class="stat"><div class="val" id="d-bat">—</div><div class="lbl">电量 %</div></div>
        <div class="stat"><div class="val" id="d-temp">—</div><div class="lbl">温度 ℃</div></div>
        <div class="stat"><div class="val" id="d-hum">—</div><div class="lbl">湿度 %</div></div>
        <div class="stat"><div class="val" id="d-wind">—</div><div class="lbl">风速 m/s</div></div>
        <div class="stat"><div class="val" id="d-rain">—</div><div class="lbl">降雨</div></div>
        <div class="stat"><div class="val" id="d-cov">—</div><div class="lbl">舱门</div></div>
        <div class="stat"><div class="val" id="d-sig">—</div><div class="lbl">4G 信号</div></div>
        <div class="stat"><div class="val" id="d-mode">—</div><div class="lbl">模式</div></div>
      </div>
      <div id="dock-extra" style="display:none;font-size:12px;color:#8b949e;line-height:2;margin-top:10px">
        <div>Dock SN：<span style="color:#e6edf3" id="d-dsn">—</span></div>
        <div>飞机 SN：<span style="color:#e6edf3" id="d-asn">—</span></div>
        <div>经/纬/海拔：<span style="color:#e6edf3" id="d-pos">—</span></div>
      </div>
    </div>
  </div>

  <!-- 网络诊断 -->
  <div class="card">
    <div class="card-header"><h2>网络诊断</h2><button class="btn btn-primary btn-sm" onclick="fetchNet()">重新检测</button></div>
    <div class="card-body">
      <div style="font-size:12px;color:#8b949e;line-height:2">
        <div>本机网卡：<span style="color:#e6edf3" id="n-iface">—</span></div>
        <div>本机 IP：<span style="color:#e6edf3" id="n-ip">—</span></div>
        <div>是否在 Dock 网段 (192.168.200.x)：<span id="n-subnet">—</span></div>
        <div>能否 ping 通 Dock (192.168.200.100)：<span id="n-ping">—</span></div>
      </div>
    </div>
  </div>

  <!-- 云消息发送 -->
  <div class="card">
    <div class="card-header"><h2>云消息通道</h2></div>
    <div class="card-body">
      <div id="cloud-log" class="log-box"></div>
      <div class="send-row">
        <input id="msg-input" type="text" placeholder="输入发送给 Dock2 的消息（最多256字节）" maxlength="256"/>
        <button class="btn btn-primary" id="send-btn" onclick="sendMessage()">发送</button>
      </div>
    </div>
  </div>

  <!-- 实时日志 -->
  <div class="card" style="grid-column:1/-1">
    <div class="card-header">
      <h2>实时日志</h2>
      <button class="btn btn-danger btn-sm" onclick="clearLog()">清空</button>
    </div>
    <div class="card-body">
      <div id="main-log" class="log-box" style="height:260px"></div>
    </div>
  </div>

  <!-- 媒体文件 -->
  <div class="card" style="grid-column:1/-1">
    <div class="card-header">
      <h2>媒体文件列表</h2>
      <button class="btn btn-primary btn-sm" onclick="fetchMedia()">获取列表</button>
    </div>
    <div class="card-body">
      <div id="file-list" class="file-list">
        <div style="padding:20px;text-align:center;color:#484f58;font-size:13px">点击"获取列表"拉取 Dock2 媒体文件</div>
      </div>
    </div>
  </div>
</div>

<div id="toast"></div>

<script>
let logLines = [];
let autoScroll = true;

function showToast(msg, ok=true){
  const t=document.getElementById('toast');
  t.textContent=msg;
  t.style.background=ok?'#1f6feb':'#da3633';
  t.style.opacity='1';
  setTimeout(()=>t.style.opacity='0',2500);
}

async function fetchStatus(){
  try{
    const r=await fetch('/api/status');
    const d=await r.json();
    const badge=document.getElementById('sdk-badge');
    if(d.sdk_init_ok){
      badge.textContent='SDK 已初始化';
      badge.className='badge ok';
      badge.classList.remove('pulse');
    } else if(d.sdk_init_pending){
      badge.textContent='SDK 初始化中…';
      badge.className='badge warn pulse';
    } else {
      badge.textContent='SDK 初始化失败 (err '+d.sdk_init_error+')';
      badge.className='badge err';
      badge.classList.remove('pulse');
    }
    document.getElementById('stat-conn').textContent =
      d.sdk_init_ok ? '✓ 已连接'
                    : (d.sdk_init_pending ? '⏳ 等待' : '✗ 失败');
    document.getElementById('stat-files').textContent = d.media_count;
    document.getElementById('stat-msgs').textContent = d.cloud_msg_count;
  }catch(e){}
}

async function fetchLogs(){
  try{
    const r=await fetch('/api/logs');
    const d=await r.json();
    logLines=d.lines||[];
    const box=document.getElementById('main-log');
    box.innerHTML=logLines.map(l=>{
      let cls='info';
      if(l.includes('[Warn]')||l.includes('warn')) cls='warn';
      if(l.includes('[Error]')||l.includes('error')||l.includes('failed')) cls='err';
      return `<span class="${cls}">${escHtml(l)}</span>`;
    }).join('');
    if(autoScroll) box.scrollTop=box.scrollHeight;
  }catch(e){}
}

async function fetchCloudMsgs(){
  try{
    const r=await fetch('/api/cloud_messages');
    const d=await r.json();
    const box=document.getElementById('cloud-log');
    const msgs=d.messages||[];
    box.innerHTML=msgs.map(m=>`<span class="info">${escHtml(m)}</span>`).join('');
    box.scrollTop=box.scrollHeight;
  }catch(e){}
}

async function fetchMedia(){
  try{
    const r=await fetch('/api/media');
    const d=await r.json();
    const el=document.getElementById('file-list');
    if(!d.files||d.files.length===0){
      el.innerHTML='<div style="padding:20px;text-align:center;color:#484f58;font-size:13px">暂无媒体文件（确认 Dock2 已连接）</div>';
      return;
    }
    el.innerHTML=d.files.map(f=>{
      const isVideo=f.toLowerCase().includes('.mp4');
      const tag=isVideo?'<span class="tag tag-mp4">MP4</span>':'<span class="tag tag-jpeg">JPEG</span>';
      return `<div class="file-item">${tag}<span>${escHtml(f)}</span></div>`;
    }).join('');
    showToast(`获取到 ${d.files.length} 个文件`);
  }catch(e){showToast('获取失败',false);}
}

async function sendMessage(){
  const input=document.getElementById('msg-input');
  const msg=input.value.trim();
  if(!msg) return;
  const btn=document.getElementById('send-btn');
  btn.disabled=true;
  try{
    const r=await fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({message:msg})});
    const d=await r.json();
    if(d.ok){
      showToast('发送成功');
      input.value='';
    } else {
      showToast('发送失败: '+d.error, false);
    }
  }catch(e){showToast('发送错误',false);}
  btn.disabled=false;
}

function clearLog(){
  fetch('/api/logs/clear',{method:'POST'});
  logLines=[];
  document.getElementById('main-log').innerHTML='';
}

async function fetchDock(){
  try{
    const r=await fetch('/api/dock');
    const d=await r.json();
    if(!d.valid){
      document.getElementById('dock-empty').style.display='block';
      document.getElementById('dock-grid').style.display='none';
      document.getElementById('dock-extra').style.display='none';
      document.getElementById('dock-updated').textContent='—';
      return;
    }
    document.getElementById('dock-empty').style.display='none';
    document.getElementById('dock-grid').style.display='flex';
    document.getElementById('dock-extra').style.display='block';
    document.getElementById('dock-updated').textContent =
      (d.from_mock?'[模拟] ':'') + '更新于 '+d.updated_at;
    document.getElementById('d-bat').textContent  = d.battery_pct<0?'—':d.battery_pct;
    document.getElementById('d-temp').textContent = d.temperature.toFixed(1);
    document.getElementById('d-hum').textContent  = d.humidity<0?'—':d.humidity;
    document.getElementById('d-wind').textContent = d.wind_speed.toFixed(1);
    const rainMap = ['无','小雨','中雨','大雨'];
    document.getElementById('d-rain').textContent = d.rainfall<0?'—':(rainMap[d.rainfall]||d.rainfall);
    const covMap = ['关闭','开启','半开'];
    document.getElementById('d-cov').textContent  = d.cover_state<0?'—':(covMap[d.cover_state]||d.cover_state);
    document.getElementById('d-sig').textContent  = d.signal_4g<0?'—':d.signal_4g;
    document.getElementById('d-mode').textContent = d.mode||'—';
    document.getElementById('d-dsn').textContent  = d.dock_sn||'—';
    document.getElementById('d-asn').textContent  = d.aircraft_sn||'—';
    document.getElementById('d-pos').textContent  =
      `${d.longitude.toFixed(6)}, ${d.latitude.toFixed(6)}, ${d.altitude.toFixed(1)} m`;
  }catch(e){}
}

async function fetchNet(){
  try{
    const r=await fetch('/api/net');
    const d=await r.json();
    document.getElementById('n-iface').textContent = d.iface||'(none)';
    document.getElementById('n-ip').textContent    = d.ip||'(none)';
    const sn=document.getElementById('n-subnet');
    sn.textContent = d.in_dock_subnet?'✓ 是':'✗ 否（需配置 192.168.200.x）';
    sn.style.color = d.in_dock_subnet?'#3fb950':'#f85149';
    const pg=document.getElementById('n-ping');
    pg.textContent = d.dock_pingable?'✓ 通':'✗ 不通';
    pg.style.color = d.dock_pingable?'#3fb950':'#f85149';
  }catch(e){}
}

async function injectMock(){
  try{
    const r=await fetch('/api/dock/inject',{method:'POST'});
    const d=await r.json();
    if(d.ok){ showToast('已注入示例数据'); fetchDock(); }
    else showToast('注入失败',false);
  }catch(e){showToast('注入错误',false);}
}

function refreshAll(){
  fetchStatus();fetchLogs();fetchCloudMsgs();fetchDock();
}

function escHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

document.getElementById('msg-input').addEventListener('keydown',e=>{
  if(e.key==='Enter') sendMessage();
});

// Poll every 2s
setInterval(()=>{fetchStatus();fetchLogs();fetchCloudMsgs();fetchDock();},2000);
// Network check is more expensive (runs `ping`), poll slower
setInterval(fetchNet, 10000);
refreshAll();
fetchNet();
</script>
</body>
</html>
)HTML";

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else out += c;
    }
    out += "\"";
    return out;
}

// ─── HTTP Server setup ────────────────────────────────────────────────────────

static void setupRoutes(httplib::Server& srv) {

    // Main page
    srv.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML, "text/html; charset=utf-8");
    });

    // Status JSON
    srv.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream ss;
        int media_count = 0;
        {
            std::lock_guard<std::mutex> lk(g_state.media_mutex);
            media_count = (int)g_state.media_file_names.size();
        }
        int cloud_count = 0;
        {
            std::lock_guard<std::mutex> lk(g_state.cloud_mutex);
            cloud_count = (int)g_state.cloud_messages.size();
        }
        ss << "{"
           << "\"sdk_init_ok\":" << (g_state.sdk_init_ok ? "true" : "false") << ","
           << "\"sdk_init_pending\":" << (g_state.sdk_init_pending ? "true" : "false") << ","
           << "\"sdk_init_error\":" << g_state.sdk_init_error.load() << ","
           << "\"sdk_connected\":" << (g_state.sdk_connected ? "true" : "false") << ","
           << "\"media_count\":" << media_count << ","
           << "\"cloud_msg_count\":" << cloud_count
           << "}";
        res.set_content(ss.str(), "application/json");
    });

    // Log lines JSON
    srv.Get("/api/logs", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream ss;
        ss << "{\"lines\":[";
        std::lock_guard<std::mutex> lk(g_state.log_mutex);
        bool first = true;
        for (const auto& line : g_state.log_lines) {
            if (!first) ss << ",";
            ss << jsonStr(line);
            first = false;
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // Clear logs
    srv.Post("/api/logs/clear", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_state.log_mutex);
        g_state.log_lines.clear();
        res.set_content("{\"ok\":true}", "application/json");
    });

    // Cloud messages
    srv.Get("/api/cloud_messages", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream ss;
        ss << "{\"messages\":[";
        std::lock_guard<std::mutex> lk(g_state.cloud_mutex);
        bool first = true;
        for (const auto& msg : g_state.cloud_messages) {
            if (!first) ss << ",";
            ss << jsonStr(msg);
            first = false;
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // Send message to cloud
    srv.Post("/api/send", [](const httplib::Request& req, httplib::Response& res) {
        // Simple JSON parse for {"message":"..."}
        auto body = req.body;
        auto pos = body.find("\"message\"");
        if (pos == std::string::npos) {
            res.set_content("{\"ok\":false,\"error\":\"missing message field\"}", "application/json");
            return;
        }
        auto q1 = body.find('"', pos + 9);
        if (q1 == std::string::npos) {
            res.set_content("{\"ok\":false,\"error\":\"parse error\"}", "application/json");
            return;
        }
        auto q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos) {
            res.set_content("{\"ok\":false,\"error\":\"parse error\"}", "application/json");
            return;
        }
        std::string msg = body.substr(q1 + 1, q2 - q1 - 1);
        if (msg.size() > 256) msg = msg.substr(0, 256);

        auto rc = CloudAPI_SendCustomEventsMessage(
            reinterpret_cast<const uint8_t*>(msg.c_str()), msg.size());
        if (rc == kOk) {
            g_state.pushLog("[Cloud→Dock] " + msg);
            res.set_content("{\"ok\":true}", "application/json");
        } else {
            std::string err = "{\"ok\":false,\"error\":\"sdk error " + std::to_string(rc) + "\"}";
            res.set_content(err, "application/json");
        }
    });

    // Media file list
    srv.Get("/api/media", [](const httplib::Request&, httplib::Response& res) {
        // Refresh from SDK
        {
            auto reader = MediaManager::Instance()->CreateMediaFilesReader();
            if (reader->Init() == kOk) {
                MediaFilesReader::MediaFileList list;
                int n = reader->FileList(list);
                if (n > 0) {
                    std::lock_guard<std::mutex> lk(g_state.media_mutex);
                    g_state.media_file_names.clear();
                    for (const auto& f : list) {
                        g_state.media_file_names.push_back(
                            f->file_name + "  (" + std::to_string(f->file_size / 1024) + " KB)");
                    }
                }
                reader->DeInit();
            }
        }
        std::ostringstream ss;
        ss << "{\"files\":[";
        std::lock_guard<std::mutex> lk(g_state.media_mutex);
        bool first = true;
        for (const auto& name : g_state.media_file_names) {
            if (!first) ss << ",";
            ss << jsonStr(name);
            first = false;
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // Dock status JSON
    srv.Get("/api/dock", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_state.dock_mutex);
        const DockStatus& d = g_state.dock;
        std::ostringstream ss;
        ss << "{"
           << "\"valid\":"       << (d.valid ? "true" : "false") << ","
           << "\"from_mock\":"   << (d.from_mock ? "true" : "false") << ","
           << "\"updated_at\":"  << jsonStr(d.updated_at) << ","
           << "\"dock_sn\":"     << jsonStr(d.dock_sn) << ","
           << "\"aircraft_sn\":" << jsonStr(d.aircraft_sn) << ","
           << "\"battery_pct\":" << d.battery_pct << ","
           << "\"temperature\":" << d.temperature << ","
           << "\"humidity\":"    << d.humidity << ","
           << "\"wind_speed\":"  << d.wind_speed << ","
           << "\"rainfall\":"    << d.rainfall << ","
           << "\"longitude\":"   << d.longitude << ","
           << "\"latitude\":"    << d.latitude << ","
           << "\"altitude\":"    << d.altitude << ","
           << "\"mode\":"        << jsonStr(d.mode) << ","
           << "\"cover_state\":" << d.cover_state << ","
           << "\"signal_4g\":"   << d.signal_4g
           << "}";
        res.set_content(ss.str(), "application/json");
    });

    // Inject a sample status payload so the UI can be checked before the Dock
    // is online. Accepts a custom payload via POST body if you want to test
    // the JSON parser.
    srv.Post("/api/dock/inject", [](const httplib::Request& req,
                                    httplib::Response& res) {
        std::string payload = req.body;
        if (payload.empty()) {
            payload =
                "{\"type\":\"dock_osd\","
                "\"dock_sn\":\"5TADKAAAAAAAAA\","
                "\"aircraft_sn\":\"1581F0AAAAAAAA\","
                "\"battery\":87,\"temperature\":26.4,\"humidity\":58,"
                "\"wind_speed\":2.6,\"rainfall\":0,"
                "\"lon\":113.937412,\"lat\":22.532315,\"alt\":18.5,"
                "\"mode\":\"idle\",\"cover\":0,\"signal_4g\":4}";
        }
        applyDockJson(payload);
        {
            std::lock_guard<std::mutex> lk(g_state.dock_mutex);
            g_state.dock.from_mock = true;
        }
        g_state.pushLog("[Mock] Dock status injected");
        res.set_content("{\"ok\":true}", "application/json");
    });

    // Network diagnostics
    srv.Get("/api/net", [](const httplib::Request&, httplib::Response& res) {
        NetInfo n = collectNetInfo();
        std::ostringstream ss;
        ss << "{"
           << "\"iface\":"           << jsonStr(n.iface) << ","
           << "\"ip\":"              << jsonStr(n.ip) << ","
           << "\"in_dock_subnet\":"  << (n.in_dock_subnet ? "true" : "false") << ","
           << "\"dock_pingable\":"   << (n.dock_pingable ? "true" : "false")
           << "}";
        res.set_content(ss.str(), "application/json");
    });
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) port = std::atoi(argv[1]);

    // Run ESDK init on a background thread so the dashboard stays responsive
    // even while the SDK is still searching for the Dock on 192.168.200.x.
    std::thread([]{
        g_state.pushLog("[DockerHelp] 正在初始化 ESDK...");
        auto rc = ESDKInit();
        g_state.sdk_init_pending = false;
        g_state.sdk_init_error = (int)rc;
        if (rc != kOk) {
            g_state.pushLog("[DockerHelp] ESDK 初始化失败，错误码: " + std::to_string(rc));
            g_state.sdk_init_ok = false;
            return;
        }
        g_state.sdk_init_ok = true;
        g_state.pushLog("[DockerHelp] ESDK 初始化成功，等待 Dock 连接...");
        CloudAPI_RegisterCustomServicesMessageHandler(OnCloudMessage);
        MediaManager::Instance()->RegisterMediaFilesObserver(OnNewMediaFile);
    }).detach();

    // HTTP server starts immediately
    httplib::Server srv;
    setupRoutes(srv);

    g_state.pushLog("[DockerHelp] Web 服务启动，访问 http://localhost:" + std::to_string(port));
    printf("\n==============================================\n");
    printf("  DockerHelp 已启动 (Dock 期望网段: 192.168.200.x)\n");
    printf("  浏览器访问: http://localhost:%d\n", port);
    printf("==============================================\n\n");

    srv.listen("0.0.0.0", port);
    return 0;
}
