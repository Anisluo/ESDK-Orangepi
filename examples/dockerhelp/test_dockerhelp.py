#!/usr/bin/env python3
"""
dockerhelp 一键测试脚本。

用法:
    python3 test_dockerhelp.py 192.168.200.55          # 默认端口 8080
    python3 test_dockerhelp.py 192.168.200.55:8080
    python3 test_dockerhelp.py 192.168.200.55 --with-mock
    python3 test_dockerhelp.py 192.168.200.55 --json report.json

覆盖项:
    1. L3 连通性: ping, TCP 8080 可达, HTTP 根页面
    2. 所有 /api/* 端点: 字段存在性 + 类型 + 取值范围
    3. 边界 case: 超长云消息(>256B)、错误 JSON、不存在的路由

默认 **只读** —— 不会改变 dockerhelp 状态。开 --with-mock 才会
POST /api/dock/inject 和 /api/send。
"""

import argparse
import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any


# ─── Pretty output ───────────────────────────────────────────────────────────

USE_COLOR = sys.stdout.isatty()


def c(code: str, s: str) -> str:
    return f"\033[{code}m{s}\033[0m" if USE_COLOR else s


GREEN = lambda s: c("32", s)
RED = lambda s: c("31", s)
YELLOW = lambda s: c("33", s)
DIM = lambda s: c("2", s)
BOLD = lambda s: c("1", s)


class Report:
    def __init__(self):
        self.cases: list[dict[str, Any]] = []

    def add(self, name: str, ok: bool, detail: str = "", skipped: bool = False):
        status = "SKIP" if skipped else ("PASS" if ok else "FAIL")
        self.cases.append({"name": name, "status": status, "detail": detail})
        tag = (
            YELLOW("SKIP") if skipped else (GREEN("PASS") if ok else RED("FAIL"))
        )
        print(f"  [{tag}] {name}" + (f"  {DIM(detail)}" if detail else ""))

    def summary(self) -> tuple[int, int, int]:
        passed = sum(1 for c in self.cases if c["status"] == "PASS")
        failed = sum(1 for c in self.cases if c["status"] == "FAIL")
        skipped = sum(1 for c in self.cases if c["status"] == "SKIP")
        return passed, failed, skipped


# ─── HTTP helpers (stdlib only) ──────────────────────────────────────────────


def http(
    method: str,
    url: str,
    body: bytes | None = None,
    timeout: float = 5.0,
) -> tuple[int, dict, bytes]:
    req = urllib.request.Request(url, data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    # bypass proxy for LAN addresses
    handler = urllib.request.ProxyHandler({})
    opener = urllib.request.build_opener(handler)
    try:
        with opener.open(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def get_json(url: str, timeout: float = 5.0) -> tuple[int, Any]:
    code, _, body = http("GET", url, timeout=timeout)
    try:
        return code, json.loads(body.decode("utf-8"))
    except Exception:
        return code, None


def post_json(url: str, payload: Any, timeout: float = 5.0) -> tuple[int, Any]:
    body = json.dumps(payload).encode("utf-8") if payload is not None else b""
    code, _, raw = http("POST", url, body=body, timeout=timeout)
    try:
        return code, json.loads(raw.decode("utf-8"))
    except Exception:
        return code, None


# ─── Section 1: L3 reachability ──────────────────────────────────────────────


def test_l3(r: Report, host: str, port: int) -> bool:
    print(BOLD("\n[1/3] L3 连通性"))

    # ping (1 packet, 2s timeout)
    rc = subprocess.run(
        ["ping", "-c", "1", "-W", "2", host],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode
    r.add(f"ping {host}", rc == 0, "" if rc == 0 else "host 不通,后续测试会失败")

    if rc != 0:
        r.add(f"TCP {host}:{port}", False, "ping 都不通,跳过 TCP", skipped=True)
        r.add("HTTP /", False, "host 不通,跳过 HTTP", skipped=True)
        return False

    # TCP connect
    tcp_ok = False
    try:
        with socket.create_connection((host, port), timeout=3):
            tcp_ok = True
    except Exception as e:
        r.add(f"TCP {host}:{port}", False, f"{type(e).__name__}: {e}")
    if tcp_ok:
        r.add(f"TCP {host}:{port}", True, "可连接")

    if not tcp_ok:
        r.add("HTTP /", False, "TCP 不通,跳过", skipped=True)
        return False

    # HTTP root
    try:
        code, _, body = http("GET", f"http://{host}:{port}/", timeout=5)
        ok = code == 200 and b"DockerHelp" in body
        r.add(
            "HTTP / (主页)",
            ok,
            f"HTTP {code}, {len(body)} bytes"
            + ("" if ok else ", 主页内容不对"),
        )
    except Exception as e:
        r.add("HTTP / (主页)", False, f"{type(e).__name__}: {e}")
        return False

    return True


# ─── Section 2: /api/* endpoint contract ─────────────────────────────────────

NUMERIC = (int, float)


def check_fields(
    r: Report, name: str, data: Any, schema: dict[str, tuple[type | tuple, str]]
):
    """schema: {field: (type-or-tuple, human-meaning)}"""
    if not isinstance(data, dict):
        r.add(name, False, f"返回不是 JSON 对象: {type(data).__name__}")
        return
    missing = [k for k in schema if k not in data]
    if missing:
        r.add(name, False, f"缺少字段: {missing}")
        return
    bad = []
    for k, (t, _) in schema.items():
        if not isinstance(data[k], t):
            bad.append(f"{k}={data[k]!r} (期望 {t})")
    if bad:
        r.add(name, False, "类型错: " + "; ".join(bad))
        return
    r.add(name, True, f"字段齐 ({len(schema)} 项)")


def test_api(r: Report, base: str) -> dict[str, Any]:
    print(BOLD("\n[2/3] /api/* 端点"))

    results: dict[str, Any] = {}

    # /api/status
    code, data = get_json(f"{base}/api/status")
    results["status"] = data
    if code != 200 or data is None:
        r.add("/api/status", False, f"HTTP {code} 或非 JSON")
    else:
        check_fields(
            r,
            "/api/status",
            data,
            {
                "sdk_init_ok": (bool, "SDK 是否初始化成功"),
                "sdk_init_pending": (bool, "是否还在初始化"),
                "sdk_init_error": (int, "错误码"),
                "sdk_connected": (bool, "Dock 是否连接"),
                "media_count": (int, "媒体文件数"),
                "cloud_msg_count": (int, "云消息数"),
            },
        )
        # additional sanity
        if (
            data.get("sdk_init_ok")
            and data.get("sdk_init_pending")
        ):
            r.add(
                "/api/status 一致性",
                False,
                "init_ok=true 但 pending=true 矛盾",
            )
        else:
            r.add("/api/status 一致性", True)

    # /api/net
    code, data = get_json(f"{base}/api/net", timeout=8)  # ping inside
    results["net"] = data
    if code != 200 or data is None:
        r.add("/api/net", False, f"HTTP {code} 或非 JSON")
    else:
        check_fields(
            r,
            "/api/net",
            data,
            {
                "iface": (str, "本机网卡"),
                "ip": (str, "本机 IP"),
                "in_dock_subnet": (bool, "是否在 192.168.200.x"),
                "dock_pingable": (bool, "能否 ping 通 Dock"),
            },
        )
        print(
            DIM(
                f"        ↳ 本机 {data.get('iface')}={data.get('ip')}, "
                f"on_dock_subnet={data.get('in_dock_subnet')}, "
                f"dock_ping={data.get('dock_pingable')}"
            )
        )

    # /api/dock (current state, may be invalid)
    code, data = get_json(f"{base}/api/dock")
    results["dock"] = data
    if code != 200 or data is None:
        r.add("/api/dock", False, f"HTTP {code} 或非 JSON")
    else:
        check_fields(
            r,
            "/api/dock",
            data,
            {
                "valid": (bool, "是否有数据"),
                "from_mock": (bool, "是否是 mock 数据"),
                "updated_at": (str, "更新时间"),
                "dock_sn": (str, "Dock 序列号"),
                "aircraft_sn": (str, "飞机序列号"),
                "battery_pct": (int, "电量百分比"),
                "temperature": (NUMERIC, "温度"),
                "humidity": (int, "湿度"),
                "wind_speed": (NUMERIC, "风速"),
                "rainfall": (int, "降雨等级"),
                "longitude": (NUMERIC, "经度"),
                "latitude": (NUMERIC, "纬度"),
                "altitude": (NUMERIC, "海拔"),
                "mode": (str, "工作模式"),
                "cover_state": (int, "舱门状态"),
                "signal_4g": (int, "4G 信号"),
            },
        )

    # /api/logs
    code, data = get_json(f"{base}/api/logs")
    results["logs"] = data
    if code != 200 or not isinstance(data, dict) or not isinstance(
        data.get("lines"), list
    ):
        r.add("/api/logs", False, f"HTTP {code} 或 lines 不是数组")
    else:
        r.add("/api/logs", True, f"{len(data['lines'])} 条日志")

    # /api/cloud_messages
    code, data = get_json(f"{base}/api/cloud_messages")
    results["cloud_messages"] = data
    if code != 200 or not isinstance(data, dict) or not isinstance(
        data.get("messages"), list
    ):
        r.add(
            "/api/cloud_messages",
            False,
            f"HTTP {code} 或 messages 不是数组",
        )
    else:
        r.add(
            "/api/cloud_messages",
            True,
            f"{len(data['messages'])} 条云消息",
        )

    # /api/media (slow: hits SDK to refresh)
    print(DIM("        ↳ /api/media 会调用 SDK 拉文件列表,稍等..."))
    code, data = get_json(f"{base}/api/media", timeout=15)
    results["media"] = data
    if code != 200 or not isinstance(data, dict) or not isinstance(
        data.get("files"), list
    ):
        r.add("/api/media", False, f"HTTP {code} 或 files 不是数组")
    else:
        sdk_ok = (results.get("status") or {}).get("sdk_init_ok")
        note = f"{len(data['files'])} 个文件"
        if not sdk_ok and data["files"]:
            note += " (SDK 未初始化但有文件?)"
        r.add("/api/media", True, note)

    return results


# ─── Section 3: Edge cases ───────────────────────────────────────────────────


def test_edge_cases(r: Report, base: str, with_mock: bool):
    print(BOLD("\n[3/3] 边界 case"))

    # Unknown route → expect 404
    try:
        code, _, _ = http("GET", f"{base}/api/no-such-route", timeout=3)
        r.add(
            "GET /api/no-such-route (404)",
            code == 404,
            f"返回 {code}" + ("" if code == 404 else ", 期望 404"),
        )
    except Exception as e:
        r.add("GET /api/no-such-route (404)", False, f"{e}")

    # Wrong method on a GET endpoint → expect non-200
    try:
        code, _, _ = http("POST", f"{base}/api/status", body=b"x", timeout=3)
        r.add(
            "POST /api/status (方法不对)",
            code != 200,
            f"返回 {code}",
        )
    except Exception as e:
        r.add("POST /api/status (方法不对)", True, f"{type(e).__name__} (符合预期)")

    if not with_mock:
        r.add(
            "POST /api/dock/inject (错误 JSON)",
            False,
            "默认只读模式,跳过 (用 --with-mock 启用)",
            skipped=True,
        )
        r.add(
            "POST /api/send (>256 字节)",
            False,
            "默认只读模式,跳过 (用 --with-mock 启用)",
            skipped=True,
        )
        return

    # --- writes below only run with --with-mock ---

    # Garbage body to /api/dock/inject → should still 200 (no fields parsed)
    code, data = post_json(f"{base}/api/dock/inject", "not-json")
    r.add(
        "POST /api/dock/inject (非 JSON body)",
        code == 200 and isinstance(data, dict) and data.get("ok") is True,
        f"返回 {code} {data}",
    )

    # Valid inject + readback
    payload = {
        "type": "dock_osd",
        "battery": 73,
        "temperature": 18.2,
        "mode": "test_probe",
    }
    code, _ = post_json(f"{base}/api/dock/inject", payload)
    if code != 200:
        r.add("POST /api/dock/inject (有效)", False, f"HTTP {code}")
    else:
        time.sleep(0.2)
        _, dock = get_json(f"{base}/api/dock")
        ok = (
            isinstance(dock, dict)
            and dock.get("battery_pct") == 73
            and abs(dock.get("temperature", 0) - 18.2) < 0.01
            and dock.get("mode") == "test_probe"
        )
        r.add(
            "Mock 注入回读 (battery=73, mode=test_probe)",
            ok,
            "" if ok else f"实际: battery={dock and dock.get('battery_pct')}, "
            f"mode={dock and dock.get('mode')}",
        )

    # Oversize cloud message: dockerhelp truncates to 256 bytes before
    # calling SDK. We expect either ok=true (SDK accepts truncated) or
    # ok=false with a sane error — never a crash / 5xx.
    big = "A" * 1024
    try:
        code, data = post_json(f"{base}/api/send", {"message": big}, timeout=5)
        ok = code == 200 and isinstance(data, dict) and "ok" in data
        r.add(
            "POST /api/send (1024 字节)",
            ok,
            f"HTTP {code} {data}",
        )
    except Exception as e:
        r.add("POST /api/send (1024 字节)", False, f"{e}")

    # Malformed /api/send body (no "message" field)
    code, data = post_json(f"{base}/api/send", {"foo": "bar"})
    ok = (
        code == 200
        and isinstance(data, dict)
        and data.get("ok") is False
        and "error" in data
    )
    r.add(
        "POST /api/send (缺 message 字段)",
        ok,
        f"返回 {data}",
    )


# ─── Main ────────────────────────────────────────────────────────────────────


def parse_target(s: str) -> tuple[str, int]:
    if ":" in s:
        host, port = s.rsplit(":", 1)
        return host, int(port)
    return s, 8080


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__,
    )
    ap.add_argument("target", help="dockerhelp 服务地址,如 192.168.200.55 或 host:port")
    ap.add_argument(
        "--with-mock",
        action="store_true",
        help="启用会改状态的测试 (POST /api/dock/inject 和 /api/send)",
    )
    ap.add_argument(
        "--json",
        metavar="FILE",
        help="把详细结果写入 JSON 文件",
    )
    args = ap.parse_args()

    host, port = parse_target(args.target)
    base = f"http://{host}:{port}"

    print(BOLD(f"\nDockerHelp 一键测试  目标: {base}"))
    if args.with_mock:
        print(YELLOW("  --with-mock 已启用: 会注入测试数据,影响 UI 显示"))
    else:
        print(DIM("  默认只读模式 (不改状态), 加 --with-mock 启用写入测试"))

    r = Report()
    t0 = time.time()

    reachable = test_l3(r, host, port)
    api_results: dict[str, Any] = {}
    if reachable:
        api_results = test_api(r, base)
        test_edge_cases(r, base, args.with_mock)
    else:
        print(RED("\n  L3 不通,跳过后续测试"))

    elapsed = time.time() - t0
    passed, failed, skipped = r.summary()

    print(BOLD("\n══════════════════════════════════════════════"))
    print(
        f"  {GREEN(f'{passed} PASS')}  "
        f"{RED(f'{failed} FAIL') if failed else f'{failed} FAIL'}  "
        f"{YELLOW(f'{skipped} SKIP')}  "
        f"耗时 {elapsed:.1f}s"
    )
    print(BOLD("══════════════════════════════════════════════\n"))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "target": base,
                    "elapsed_sec": round(elapsed, 2),
                    "summary": {
                        "pass": passed,
                        "fail": failed,
                        "skip": skipped,
                    },
                    "cases": r.cases,
                    "snapshot": api_results,
                },
                f,
                ensure_ascii=False,
                indent=2,
            )
        print(DIM(f"  详细报告: {args.json}\n"))

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
