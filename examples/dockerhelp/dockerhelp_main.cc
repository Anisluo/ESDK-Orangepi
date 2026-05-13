/**
 * DJI Dock2 Debug Assistant - DockerHelp
 * Web-based debug dashboard for DJI Edge SDK
 * Access via browser: http://localhost:8080
 */
#include <unistd.h>
#include <atomic>
#include <chrono>
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

struct AppState {
    std::atomic<bool>        sdk_connected{false};
    std::atomic<bool>        sdk_init_ok{false};
    std::string              last_cloud_msg;
    std::mutex               log_mutex;
    std::deque<std::string>  log_lines;       // rolling 200 lines
    std::deque<std::string>  cloud_messages;  // rolling 50 messages
    std::mutex               cloud_mutex;

    std::vector<std::string> media_file_names;
    std::mutex               media_mutex;

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

void OnCloudMessage(const uint8_t* data, uint32_t len) {
    std::string msg(reinterpret_cast<const char*>(data), len);
    g_state.pushLog("[Cloud←Dock] " + msg);
    g_state.pushCloud(msg);
}

ErrorCode OnNewMediaFile(const MediaFile& f) {
    std::string info = f.file_name + "  (" +
                       std::to_string(f.file_size / 1024) + " KB)";
    g_state.pushLog("[Media] new file: " + info);
    std::lock_guard<std::mutex> lk(g_state.media_mutex);
    g_state.media_file_names.push_back(info);
    return kOk;
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
    } else {
      badge.textContent='连接中...';
      badge.className='badge warn pulse';
    }
    document.getElementById('stat-conn').textContent = d.sdk_init_ok ? '✓ 已连接' : '⏳ 等待';
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

function refreshAll(){
  fetchStatus();fetchLogs();fetchCloudMsgs();
}

function escHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

document.getElementById('msg-input').addEventListener('keydown',e=>{
  if(e.key==='Enter') sendMessage();
});

// Poll every 2s
setInterval(()=>{fetchStatus();fetchLogs();fetchCloudMsgs();},2000);
refreshAll();
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
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) port = std::atoi(argv[1]);

    // Init ESDK
    g_state.pushLog("[DockerHelp] 正在初始化 ESDK...");
    auto rc = ESDKInit();
    if (rc != kOk) {
        g_state.pushLog("[DockerHelp] ESDK 初始化失败，错误码: " + std::to_string(rc));
        g_state.sdk_init_ok = false;
    } else {
        g_state.sdk_init_ok = true;
        g_state.pushLog("[DockerHelp] ESDK 初始化成功，等待 Dock2 连接...");

        // Register cloud message handler
        CloudAPI_RegisterCustomServicesMessageHandler(OnCloudMessage);

        // Register media file observer
        MediaManager::Instance()->RegisterMediaFilesObserver(OnNewMediaFile);
    }

    // Start HTTP server
    httplib::Server srv;
    setupRoutes(srv);

    g_state.pushLog("[DockerHelp] Web 服务启动，访问 http://localhost:" + std::to_string(port));
    printf("\n==============================================\n");
    printf("  DockerHelp 已启动\n");
    printf("  浏览器访问: http://localhost:%d\n", port);
    printf("==============================================\n\n");

    srv.listen("0.0.0.0", port);
    return 0;
}
