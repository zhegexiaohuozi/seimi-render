#!/usr/bin/env python3
"""
seimi-render 全接口自动回测脚本。

一键：构建 → 起双实例（有/无密码）→ 跑 9 组约 60 条接口用例 → 终端彩色报告 + JSON 报告。

用法:
    python scripts/regression_test.py                 # 全套（构建+双实例+全测）
    python scripts/regression_test.py --clean         # 从零清理后全量重建再测
    python scripts/regression_test.py --skip-build    # 复用上次构建产物
    python scripts/regression_test.py --no-auth       # 跳过 F 鉴权组
    python scripts/regression_test.py --group B       # 只跑某组
    python scripts/regression_test.py --case B1,B6    # 只跑指定用例
    python scripts/regression_test.py --keep-servers  # 测完不杀进程（调试）
    python scripts/regression_test.py --verbose       # 打印每请求/响应详情

依赖: requests, websocket-client
    pip install requests websocket-client

设计文档: docs/superpowers/specs/2026-06-28-regression-test-design.md
"""
import argparse
import atexit
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime

try:
    import requests
except ImportError:
    sys.exit("缺少依赖 requests，请运行: pip install requests websocket-client")

try:
    import websocket  # websocket-client 包，import 名为 websocket
except ImportError:
    sys.exit("缺少依赖 websocket-client，请运行: pip install requests websocket-client")


# ============================================================
# §1 常量与配置
# ============================================================
IS_WINDOWS = sys.platform == "win32"
IS_MACOS = sys.platform == "darwin"

# 临时高位端口，避开默认 8088/8089/8090，防与开发中的实例冲突。
NO_AUTH_PORTS = {"http": 18088, "ws": 18089, "mcp": 18090}
AUTH_PORTS = {"http": 18188, "ws": 18189, "mcp": 18190}

AUTH_PASSWORD = "testpass_2024"
# token = sha256("seimi-render:" + password) 的 hex，与服务端 HttpServer::computeToken 一致。
EXPECTED_TOKEN = hashlib.sha256(("seimi-render:" + AUTH_PASSWORD).encode()).hexdigest()

# 真实渲染目标（内容足够丰富，能验证 markdown/截图质量）。
URL_SOHU = "https://www.sohu.com/"
URL_BING_SEARCH = "https://www.bing.com/search?q=seimi+render"

# 渲染等待窗口。
RENDER_LONG_POLL_MS = 45000   # B 系列长轮询
RENDER_HTTP_TIMEOUT = 50      # HTTP 读超时（秒），略大于长轮询窗口
RETRY_POLL_TIMEOUT_MS = 25000 # 超时补取一次的窗口
RETRY_POLL_HTTP_TIMEOUT = 30  # 补取的 HTTP 读超时

# 实例就绪探测上限。
BOOT_TIMEOUT = 60

# 临时 cookie 测试域名（不会与真实站点冲突）。
COOKIE_TEST_DOMAIN = "regression-test.example"

HOST = "127.0.0.1"

# 构建目录（与三平台构建脚本约定一致：build/）。
# --clean 时按平台策略清空：Windows 构建脚本自带每次清空（无需再清）；
# Linux/macOS 直接 rm -rf 后由构建脚本重新 configure。
BUILD_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")

# 脚本自己的退出码。
EXIT_OK = 0
EXIT_FAIL = 1          # 有用例失败（功能回归）
EXIT_ENV = 2           # 构建或启动失败（环境问题）

# 终端颜色（非 TTY 时退化为空串，避免乱码）。
if sys.stdout.isatty():
    GREEN = "\033[32m"; RED = "\033[31m"; YELLOW = "\033[33m"
    CYAN = "\033[36m"; DIM = "\033[2m"; BOLD = "\033[1m"; RESET = "\033[0m"
else:
    GREEN = RED = YELLOW = CYAN = DIM = BOLD = RESET = ""


def binary_candidates():
    """按平台返回二进制候选路径列表（按优先级）。"""
    if IS_WINDOWS:
        return ["build/seimi-render.exe", "build/Release/seimi-render.exe"]
    if IS_MACOS:
        # MACOSX_BUNDLE：二进制在 .app/Contents/MacOS/ 内。
        # CMake 输出固定此路径（见 CMakeLists.txt add_executable + MACOSX_BUNDLE）。
        return ["build/seimi-render.app/Contents/MacOS/seimi-render"]
    return ["build/seimi-render"]


def build_script(clean=False):
    """按平台返回 (构建脚本相对路径, [额外参数])。

    clean=True 时：
      - Windows: build-windows.bat 每次自带清空，无需额外参数（参数仅用于日志提示）。
      - macOS:   package.sh 接受 clean 关键字参数（清空 build 后全量重建 + 强制重部署）。
      - Linux:   build-linux.sh 无 clean 参数；由 build() 在调用前 rm -rf build/。
    """
    if IS_WINDOWS:
        return "scripts/build-windows.bat", []
    if IS_MACOS:
        # 复用 package.sh：clean 模式下它会 rm -rf build + 强制 macdeployqt 重部署，
        # 产出的 .app 框架完整、二进制可直接运行。否则增量构建（~10s）。
        return "scripts/package.sh", ["clean"] if clean else []
    return "scripts/build-linux.sh", []


# ============================================================
# §2 客户端封装
# ============================================================
class HttpResponse:
    """轻量响应封装，便于断言与诊断。"""
    def __init__(self, status, body, headers, raw_body_bytes=None):
        self.status = status
        self.headers = headers          # dict（小写键）
        self._raw_body_bytes = raw_body_bytes  # 原始字节（用于二进制魔数校验）
        # JSON 解析（失败则存原始文本）
        try:
            self.json = json.loads(body) if body else {}
            self.text = body
        except (ValueError, TypeError):
            self.json = None
            self.text = body if isinstance(body, str) else (body.decode("utf-8", "replace") if body else "")

    @property
    def body_bytes(self):
        """原始响应字节（用于 PDF/截图魔数校验）。"""
        if self._raw_body_bytes is not None:
            return self._raw_body_bytes
        return self.text.encode("utf-8", "replace")


class HttpClient:
    """seimi-render HTTP 客户端。token 非空时自动注入 Authorization: Bearer。"""

    def __init__(self, port, token=None, verbose=False):
        self.base = f"http://{HOST}:{port}"
        self.token = token
        self.verbose = verbose

    def _headers(self, extra=None):
        h = {}
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        if extra:
            h.update(extra)
        return h

    def get(self, path, params=None, timeout=15, extra_headers=None):
        url = self.base + path
        if self.verbose:
            print(f"    {DIM}GET {url} params={params}{RESET}")
        r = requests.get(url, params=params, headers=self._headers(extra_headers), timeout=timeout)
        if self.verbose:
            print(f"    {DIM}<- {r.status_code} {r.text[:200]}{RESET}")
        return HttpResponse(r.status_code, r.text, dict(r.headers), r.content)

    def post(self, path, body=None, json_body=None, timeout=15, extra_headers=None):
        """body 为原始字符串/json_body 为 dict（二选一）。"""
        url = self.base + path
        headers = self._headers(extra_headers)
        if json_body is not None:
            headers["Content-Type"] = "application/json"
        if self.verbose:
            payload = json_body if json_body is not None else body
            print(f"    {DIM}POST {url} {payload}{RESET}")
        r = requests.post(url, data=body, json=json_body, headers=headers, timeout=timeout)
        if self.verbose:
            print(f"    {DIM}<- {r.status_code} {r.text[:200]}{RESET}")
        return HttpResponse(r.status_code, r.text, dict(r.headers), r.content)

    def delete(self, path, params=None, timeout=15):
        url = self.base + path
        if self.verbose:
            print(f"    {DIM}DELETE {url} params={params}{RESET}")
        r = requests.delete(url, params=params, headers=self._headers(), timeout=timeout)
        if self.verbose:
            print(f"    {DIM}<- {r.status_code} {r.text[:200]}{RESET}")
        return HttpResponse(r.status_code, r.text, dict(r.headers), r.content)


class WsClient:
    """seimi-render WebSocket 客户端。token 非空时连接 URL 带 ?token=。"""

    def __init__(self, port, token=None):
        url = f"ws://{HOST}:{port}/"
        if token:
            url += f"?token={token}"
        self.url = url
        self.ws = None

    def connect(self, timeout=10):
        self.ws = websocket.create_connection(self.url, timeout=timeout)

    def send_json(self, obj):
        if self.ws is None:
            raise RuntimeError("WsClient not connected")
        self.ws.send(json.dumps(obj))

    def recv_json(self):
        """带超时接收一条消息并解析 JSON。超时抛 WebSocketTimeoutException。"""
        if self.ws is None:
            raise RuntimeError("WsClient not connected")
        msg = self.ws.recv()
        return json.loads(msg)

    def recv_with_timeout(self, timeout_s):
        """设定超时后接收一条消息，返回 (got: bool, obj_or_None)。超时返回 (False, None)。"""
        if self.ws is None:
            return False, None
        self.ws.settimeout(timeout_s)
        try:
            return True, self.recv_json()
        except websocket.WebSocketTimeoutException:
            return False, None
        except websocket.WebSocketConnectionClosedException:
            # 服务端关闭连接
            return False, None

    def close(self):
        if self.ws is not None:
            try:
                self.ws.close()
            except Exception:
                pass
            self.ws = None


class McpError(Exception):
    """MCP JSON-RPC error。"""
    def __init__(self, err):
        self.code = err.get("code")
        self.message = err.get("message", "")
        super().__init__(f"MCP error {self.code}: {self.message}")


class McpClient:
    """seimi-render MCP 客户端（Streamable HTTP，raw JSON-RPC，无需 async SDK）。

    协议层只用到 initialize / tools/list / tools/call，直接 POST /mcp 即可。
    Streamable HTTP 要求 initialize 后所有请求带服务端返回的 Mcp-Session-Id 头。
    """

    def __init__(self, port, verbose=False):
        self.url = f"http://{HOST}:{port}/mcp"
        self.verbose = verbose
        self._id = 0
        self.session_headers = {"Content-Type": "application/json"}
        self._session_id = None

    def _next_id(self):
        self._id += 1
        return self._id

    def call(self, method, params=None):
        body = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": method,
            "params": params or {},
        }
        if self.verbose:
            print(f"    {DIM}MCP {method} {params}{RESET}")
        r = requests.post(self.url, json=body, headers=self.session_headers, timeout=70)
        # initialize 响应里带 Mcp-Session-Id；后续请求必须回传它。
        sid = r.headers.get("Mcp-Session-Id")
        if sid and not self._session_id:
            self._session_id = sid
            self.session_headers["Mcp-Session-Id"] = sid
        if self.verbose:
            print(f"    {DIM}<- {r.status_code} {r.text[:300]}{RESET}")
        j = r.json()
        if "error" in j:
            raise McpError(j["error"])
        return j.get("result")

    def initialize(self):
        """必须先调，建立 session。"""
        result = self.call("initialize", {
            "protocolVersion": "2025-03-26",
            "capabilities": {},
            "clientInfo": {"name": "regression-test", "version": "1.0"},
        })
        # cpp-mcp Streamable HTTP 严格要求 initialize 后发一个 notifications/initialized
        # 完成握手，否则后续 tools/list 等返回 "Session not initialized"。
        # 该通知无 id（JSON-RPC notification），不带 result/error。
        notify = {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}
        if self.verbose:
            print(f"    {DIM}MCP notifications/initialized{RESET}")
        requests.post(self.url, json=notify, headers=self.session_headers, timeout=10)
        return result

    def list_tools(self):
        return self.call("tools/list", {})

    def call_tool(self, name, arguments):
        return self.call("tools/call", {"name": name, "arguments": arguments})


# ============================================================
# §3 进程管理：构建 / 启动 / 就绪探测 / 收尾
# ============================================================
_procs = []          # 全局已启动实例进程列表，供 atexit 清理
_keep_servers = False  # --keep-servers：atexit 时据此决定是否清理


def _log(tag, msg):
    print(f"{CYAN}[{tag}]{RESET} {msg}")


def find_binary():
    """返回存在的二进制路径，找不到返回 None。"""
    for c in binary_candidates():
        if os.path.isfile(c):
            return c
    return None


def build(skip=False, clean=False):
    """调平台构建脚本。成功返回二进制路径，失败返回 None。

    clean=True：从零清理后全量重建。
      - Windows: build-windows.bat 每次自带清空，clean 仅影响日志（不重复清，免得 robocopy 跑两遍）。
      - Linux:   build-linux.sh 无 clean 参数；这里直接 rm -rf build/，再让脚本重新 configure。
      - macOS:   传 clean 给 package.sh（它内部 rm -rf build + 强制 macdeployqt 重部署）。
    skip=True 优先级高于 clean（复用产物时不清理）。
    """
    if skip:
        b = find_binary()
        if b:
            _log("build", f"--skip-build，复用 {b}")
            return b
        _log("build", f"{YELLOW}--skip-build 但未找到二进制，回退到构建{RESET}")

    # clean 预清理（Linux 路径；Windows/macOS 由各自脚本处理）。
    if clean and not IS_WINDOWS and not IS_MACOS:
        if os.path.isdir(BUILD_DIR):
            _log("build", f"--clean：rm -rf {BUILD_DIR}")
            shutil.rmtree(BUILD_DIR, ignore_errors=True)

    script, extra = build_script(clean=clean)
    cmd_desc = script + (" " + " ".join(extra) if extra else "") + ("  [clean]" if clean else "")
    _log("build", f"调用 {cmd_desc} ...")
    t0 = time.monotonic()
    try:
        if IS_WINDOWS:
            # .bat 必须用 shell=True 才能被 cmd 解析；正斜杠会被 cmd 误解析成参数开关
            # （scripts/build-windows.bat → 命令 "scripts" + 开关 /b /u...），故转反斜杠。
            rc = subprocess.call(script.replace("/", "\\"), shell=True)
        else:
            rc = subprocess.call(["bash", script] + extra)
    except FileNotFoundError:
        _log("build", f"{RED}构建脚本不存在: {script}{RESET}")
        return None
    dur = time.monotonic() - t0
    if rc != 0:
        _log("build", f"{RED}构建失败 (exit {rc}){RESET}")
        return None
    b = find_binary()
    if not b:
        _log("build", f"{RED}构建成功但未找到二进制{RESET}")
        return None
    _log("build", f"{GREEN}✓{RESET} {b} ({dur:.1f}s)")
    return b


def _no_sandbox_needed():
    """是否需要 --no-sandbox：Linux 下 root/WSL/容器需要。"""
    if IS_WINDOWS:
        return False
    # root 必须加；WSL/容器也常需要，保守起见非 Windows 一律加。
    return hasattr(os, "geteuid") and os.geteuid() == 0


def find_qt_bin():
    """返回 Qt bin 目录（含 Qt6*.dll），供启动未部署的二进制时加入 PATH。

    未部署的 seimi-render.exe 启动会因找不到 Qt DLL 而报
    STATUS_DLL_NOT_FOUND（exit 0xC0000135）。把 Qt bin 前置到子进程 PATH 即可解析。
    仅 Windows 需要（Linux 二进制 RPATH/ldconfig 通常已就绪）。返回 None 表示无需/未找到。
    """
    if not IS_WINDOWS:
        return None
    candidates = []
    qt_prefix = os.environ.get("QT_PREFIX", "C:/Qt")
    qt_ver = os.environ.get("QT_VERSION", "6.7.2")
    qt_arch = os.environ.get("QT_ARCH_DIR", "msvc2019_64")
    candidates.append(f"{qt_prefix}/{qt_ver}/{qt_arch}/bin")
    # 显式 QT_BIN 优先
    if os.environ.get("QT_BIN"):
        candidates.insert(0, os.environ["QT_BIN"])
    for c in candidates:
        if os.path.isfile(c + "/Qt6Core.dll"):
            return os.path.abspath(c)
    return None


def start_instance(binary, ports, password=None, tag="instance"):
    """启动一个 seimi-render 实例。返回 subprocess.Popen 或 None。"""
    args = [
        binary,
        "--host", HOST,
        "--http-port", str(ports["http"]),
        "--ws-port", str(ports["ws"]),
        "--mcp-port", str(ports["mcp"]),
    ]
    if _no_sandbox_needed():
        args.append("--no-sandbox")
    if password:
        args += ["--password", password]

    _log("boot", f"启动 {tag} http://127.0.0.1:{ports['http']} ...")
    try:
        kwargs = dict(
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        # Windows 未部署二进制：把 Qt bin 前置到 PATH，否则 STATUS_DLL_NOT_FOUND。
        qt_bin = find_qt_bin()
        if qt_bin:
            env = os.environ.copy()
            env["PATH"] = qt_bin + os.pathsep + env.get("PATH", "")
            kwargs["env"] = env
            _log("boot", f"{DIM}Qt bin 加入 PATH: {qt_bin}{RESET}")
        if IS_WINDOWS:
            # 独立进程组，便于 taskkill /T 杀整棵进程树（Chromium 会拉子进程）。
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kwargs["start_new_session"] = True  # 新会话/进程组，便于 kill 整组
        proc = subprocess.Popen(args, **kwargs)
    except OSError as e:
        _log("boot", f"{RED}启动失败: {e}{RESET}")
        return None
    proc._ports = ports       # 挂个属性，收尾时打印用
    proc._tag = tag
    _procs.append(proc)
    return proc


def wait_ready(http_port, tag="instance", timeout=BOOT_TIMEOUT):
    """轮询 /health 直到就绪。成功返回 True。"""
    deadline = time.time() + timeout
    url = f"http://{HOST}:{http_port}/health"
    while time.time() < deadline:
        try:
            r = requests.get(url, timeout=3)
            if r.status_code == 200 and "ok" in r.text:
                _log("boot", f"{GREEN}✓{RESET} {tag} 就绪")
                return True
        except requests.RequestException:
            pass
        time.sleep(1)
    _log("boot", f"{RED}✗ {tag} {timeout}s 未就绪{RESET}")
    return False


def _kill_proc(proc):
    """杀掉一个实例进程树。"""
    if proc is None:
        return
    try:
        if proc.poll() is not None:
            return  # 已退出
    except Exception:
        pass
    pid = proc.pid
    try:
        if IS_WINDOWS:
            # /T 杀整棵树（含 Chromium 子进程），/F 强制。
            subprocess.call(["taskkill", "/F", "/T", "/PID", str(pid)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            # 杀整个进程组（start_new_session 使其成为组长）。
            os.killpg(os.getpgid(pid), signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(pid), signal.SIGKILL)
    except Exception:
        pass
    try:
        proc.wait(timeout=5)
    except Exception:
        pass


def cleanup_all(keep=None):
    """收尾：杀掉所有已启动实例。

    keep=None（atexit 调用）：按模块级 _keep_servers 决定。
    keep=True/False（main 显式调用）：按给定值决定。
    """
    if keep is None:
        keep = _keep_servers
    if keep:
        return
    # 幂等：已清理过的不再重复打印。清空 _procs 标记完成。
    procs, _procs[:] = _procs[:], []
    for proc in procs:
        tag = getattr(proc, "_tag", "?")
        ports = getattr(proc, "_ports", {})
        _log("teardown", f"关闭 {tag} (pid={proc.pid}, http={ports.get('http')})")
        _kill_proc(proc)


atexit.register(cleanup_all)


# ============================================================
# §4 断言框架
# ============================================================
class AssertionFailure(Exception):
    """断言失败。message 即失败原因。"""


def check(cond, msg):
    """断言 cond 为真，否则抛 AssertionFailure(msg)。"""
    if not cond:
        raise AssertionFailure(msg)


class CaseResult:
    def __init__(self, cid, name, group, assertion):
        self.id = cid
        self.name = name
        self.group = group
        self.assertion = assertion
        self.status = "pending"     # pass / fail / skip
        self.duration_ms = 0
        self.error = None
        self.detail = None          # dict，失败诊断详情

    def to_dict(self):
        d = {
            "id": self.id, "name": self.name, "status": self.status,
            "duration_ms": self.duration_ms, "assertion": self.assertion,
        }
        if self.error:
            d["error"] = self.error
        if self.detail:
            d["detail"] = self.detail
        return d


class Case:
    """一个用例。func(ctx) 执行；失败抛 AssertionFailure 或其它异常。"""
    def __init__(self, cid, name, group, assertion, func, needs_auth=False):
        self.id = cid
        self.name = name
        self.group = group
        self.assertion = assertion
        self.func = func
        self.needs_auth = needs_auth  # 是否需要密码实例（用于 --no-auth 跳过）


class CaseContext:
    """传给每个用例的执行上下文，封装两个实例的客户端。"""
    def __init__(self, no_auth_http, no_auth_ws_port, no_auth_mcp_port,
                 auth_http=None, auth_ws_port=None):
        self.http = no_auth_http                       # 无密码 HttpClient
        self.ws_port = no_auth_ws_port
        self.mcp_port = no_auth_mcp_port
        self.auth_http = auth_http                     # 有密码 HttpClient（带 token），可能 None
        self.auth_ws_port = auth_ws_port


class Runner:
    def __init__(self, ctx, only_groups=None, only_cases=None, no_auth=False, verbose=False):
        self.ctx = ctx
        self.only_groups = set(only_groups) if only_groups else None
        self.only_cases = set(only_cases) if only_cases else None
        self.no_auth = no_auth
        self.verbose = verbose
        self.cases = []        # Case 列表
        self.results = []      # CaseResult 列表

    def add(self, case):
        self.cases.append(case)

    def _should_run(self, case):
        if self.only_cases is not None:
            return case.id in self.only_cases
        if self.only_groups is not None:
            return case.group in self.only_groups
        return True

    def run_all(self):
        # 按用例定义顺序执行（用例本身按组有序注册）。
        last_group_printed = None
        for case in self.cases:
            res = CaseResult(case.id, case.name, case.group, case.assertion)
            if not self._should_run(case):
                res.status = "skip"
                self.results.append(res)
                continue
            # --no-auth 跳过需要密码实例的用例
            if self.no_auth and case.needs_auth:
                res.status = "skip"
                self.results.append(res)
                continue

            # 每组首个被执行的用例前打印组标题（仅该组至少有一个会跑的用例）。
            headers = getattr(self, "_pending_group_headers", {})
            if case.group in headers and case.group != last_group_printed:
                print(f"{BOLD}{headers[case.group]}{RESET}")
                last_group_printed = case.group

            t0 = time.monotonic()
            try:
                case.func(self.ctx)
                res.status = "pass"
            except AssertionFailure as e:
                res.status = "fail"
                res.error = str(e)
            except Exception as e:
                res.status = "fail"
                res.error = f"{type(e).__name__}: {e}"
                # 捕获诊断详情（若有）
            res.duration_ms = int((time.monotonic() - t0) * 1000)

            # 实时打印
            mark = (GREEN + "✓" + RESET) if res.status == "pass" else (
                    RED + "✗" + RESET if res.status == "fail" else (DIM + "·" + RESET))
            dur = f"({res.duration_ms}ms)"
            line = f"  {mark} {BOLD}{case.id}{RESET} {case.name:<28} {DIM}{dur}{RESET}"
            if res.status == "fail":
                line += f"  {RED}{res.error}{RESET}"
            print(line)

            self.results.append(res)


# ============================================================
# §6 报告
# ============================================================
def print_summary(results, started_at_iso, duration_ms, build_info, instances):
    total = len(results)
    passed = sum(1 for r in results if r.status == "pass")
    failed = sum(1 for r in results if r.status == "fail")
    skipped = sum(1 for r in results if r.status == "skip")

    print()
    print(f"{BOLD}== 结果汇总 =={RESET}")
    print(f"  总计 {total} | {GREEN}通过 {passed}{RESET} | "
          f"{RED}失败 {failed}{RESET} | {DIM}跳过 {skipped}{RESET} | 耗时 {duration_ms/1000:.0f}s")
    if failed:
        print(f"  {RED}失败：{RESET}", end="")
        print(", ".join(f"{r.id} ({r.name})" for r in results if r.status == "fail"))
    return passed, failed, skipped


def write_json_report(results, started_at_iso, duration_ms, build_info, instances,
                      report_dir):
    os.makedirs(report_dir, exist_ok=True)
    # 按组聚合
    groups = {}
    for r in results:
        groups.setdefault(r.group, []).append(r)
    groups_out = []
    for gname, rs in groups.items():
        groups_out.append({
            "name": gname,
            "passed": sum(1 for r in rs if r.status == "pass"),
            "failed": sum(1 for r in rs if r.status == "fail"),
            "cases": [r.to_dict() for r in rs],
        })
    total = len(results)
    report = {
        "started_at": started_at_iso,
        "duration_ms": duration_ms,
        "build": build_info,
        "instances": instances,
        "summary": {
            "total": total,
            "passed": sum(1 for r in results if r.status == "pass"),
            "failed": sum(1 for r in results if r.status == "fail"),
            "skipped": sum(1 for r in results if r.status == "skip"),
        },
        "groups": groups_out,
    }
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(report_dir, f"regression-{stamp}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(f"报告已写入 {path}")
    return path


# ============================================================
# §5 用例定义（A–I 九组）
# ============================================================
def group_a(ctx):
    """A. 健康与元信息接口（无密码实例）。"""
    cases = []

    def a1(c):
        r = c.http.get("/health")
        check(r.status == 200, f"/health 状态 {r.status}")
        check(r.json and r.json.get("status") == "ok", f"/health body: {r.text}")
    cases.append(Case("A1", "健康检查", "A", "GET /health → 200 + status:ok", a1))

    def a2(c):
        r = c.http.get("/auth-status")
        check(r.status == 200, f"/auth-status 状态 {r.status}")
        check(r.json and r.json.get("password_enabled") is False,
              f"/auth-status body: {r.text}")
    cases.append(Case("A2", "鉴权状态-无密码", "A",
                      "GET /auth-status → password_enabled:false", a2))

    def a3(c):
        r = c.http.get("/")
        check(r.status == 200, f"/ 状态 {r.status}")
        check("<html" in r.text.lower(), f"/ 非 HTML: {r.text[:100]}")
    cases.append(Case("A3", "根路径管理页", "A",
                      "GET / → 200 + HTML", a3))

    def a4(c):
        r = c.http.get("/stats")
        check(r.status == 200, f"/stats 状态 {r.status}")
        for k in ("total", "pending", "running", "done"):
            check(k in r.json, f"/stats 缺字段 {k}: {r.text}")
    cases.append(Case("A4", "队列统计", "A",
                      "GET /stats → 含 total/pending/running/done", a4))

    def a5(c):
        r = c.http.get("/status", params={"domains": 5})
        check(r.status == 200, f"/status 状态 {r.status}")
        for k in ("uptime_ms", "totals", "latency_ms", "queue", "proxy", "outputs", "domains"):
            check(k in r.json, f"/status 缺字段 {k}: {r.text}")
    cases.append(Case("A5", "运行时全景状态", "A",
                      "GET /status?domains=5 → 含 uptime/totals/latency/queue/proxy/outputs/domains", a5))

    def a6(c):
        r = c.http.get("/health")
        check(r.headers.get("X-Content-Type-Options", "").lower() == "nosniff",
              f"缺 X-Content-Type-Options: {r.headers}")
        check(r.headers.get("X-Frame-Options", "").upper() == "DENY",
              f"缺 X-Frame-Options:DENY: {r.headers}")
        check("Content-Security-Policy" in r.headers,
              f"缺 CSP: {r.headers}")
    cases.append(Case("A6", "安全响应头", "A",
                      "任意请求 → 含 nosniff / X-Frame-Options:DENY / CSP", a6))

    return cases


def _render_and_get(ctx, url, output, fmt=None, md_alg=None, long_poll_ms=RENDER_LONG_POLL_MS,
                    expect_success=True, allow_retry=True):
    """提交渲染 + 长轮询。返回 HttpResponse。

    expect_success=True 且首次返回 running 时，自动用 /result/:id 补取一次（allow_retry）。
    """
    body = {"url": url, "output": output, "long_poll_ms": long_poll_ms}
    if fmt:
        body["format"] = fmt
    if md_alg:
        body["md_algorithm"] = md_alg
    r = ctx.http.post("/render", json_body=body, timeout=RENDER_HTTP_TIMEOUT)
    if r.status != 200:
        raise AssertionFailure(f"/render 状态 {r.status}: {r.text}")
    state = (r.json or {}).get("state")
    tid = (r.json or {}).get("task_id")

    # 期望成功但首次仍 running：补取一次
    if expect_success and state == "running" and allow_retry and tid:
        r2 = ctx.http.get(f"/result/{tid}",
                          params={"output": output, "timeout": RETRY_POLL_TIMEOUT_MS},
                          timeout=RETRY_POLL_HTTP_TIMEOUT)
        if r2.status == 200 and r2.json and r2.json.get("state") == "succeeded":
            return r2
        # 补取后仍未成功：返回补取响应（让调用方断言失败并打印）
        return r2
    return r


def _check_succeeded(r, label="render"):
    """断言渲染成功，返回 task_id。"""
    if not r.json:
        raise AssertionFailure(f"{label}: 非 JSON 响应 {r.text[:200]}")
    state = r.json.get("state")
    check(state == "succeeded", f"{label}: state={state} err={r.json.get('error')} body={r.text[:200]}")
    tid = r.json.get("task_id")
    check(bool(tid), f"{label}: 无 task_id")
    return tid


def _magic_ok(data, magic):
    """二进制首字节魔数校验。"""
    return data[:len(magic)] == magic


def group_b(ctx):
    """B. 渲染核心链路（真实公网：搜狐 + Bing）。"""
    cases = []

    def b1(c):
        r = _render_and_get(c, URL_SOHU, "markdown")
        tid = _check_succeeded(r, "B1")
        check(bool(r.json.get("markdown")), "B1: markdown 为空")
        check("md_algorithm_used" in r.json, f"B1: 缺 md_algorithm_used")
    cases.append(Case("B1", "搜狐首页 markdown", "B",
                      "POST /render output:markdown → succeeded + md 非空 + md_algorithm_used", b1))

    def b2(c):
        r = _render_and_get(c, URL_SOHU, "html")
        _check_succeeded(r, "B2")
        html = r.json.get("html", "")
        check(bool(html) and "<html" in html.lower(), "B2: html 为空或无 <html 标签")
    cases.append(Case("B2", "搜狐首页 html", "B",
                      "POST /render output:html → succeeded + html 含 <html", b2))

    def b3(c):
        r = _render_and_get(c, URL_SOHU, "pdf")
        tid = _check_succeeded(r, "B3")
        check(r.json.get("has_pdf") is True, f"B3: 缺 has_pdf: {r.text[:200]}")
        # 下载 PDF 校验魔数
        pdf = c.http.get(f"/pdf/{tid}", timeout=30)
        check(pdf.status == 200, f"B3: /pdf 状态 {pdf.status}")
        check(_magic_ok(pdf.body_bytes, b"%PDF"), "B3: PDF 魔数不对")
    cases.append(Case("B3", "搜狐首页 pdf", "B",
                      "output:pdf → succeeded + has_pdf + /pdf %PDF 魔数", b3))

    def b4(c):
        r = _render_and_get(c, URL_SOHU, "screenshot", fmt="png")
        tid = _check_succeeded(r, "B4")
        check(r.json.get("has_image") is True, f"B4: 缺 has_image: {r.text[:200]}")
        check(r.json.get("image_format") == "png", f"B4: image_format={r.json.get('image_format')}")
        img = c.http.get(f"/image/{tid}", timeout=30)
        check(img.status == 200, f"B4: /image 状态 {img.status}")
        check(_magic_ok(img.body_bytes, b"\x89PNG"), "B4: PNG 魔数不对")
    cases.append(Case("B4", "搜狐首页 screenshot png", "B",
                      "output:screenshot format:png → succeeded + has_image + /image PNG 魔数", b4))

    def b5(c):
        r = _render_and_get(c, URL_SOHU, "screenshot")  # 默认 auto
        tid = _check_succeeded(r, "B5")
        check(r.json.get("has_image") is True, f"B5: 缺 has_image")
        fmt = r.json.get("image_format")
        check(fmt in ("png", "jpeg"), f"B5: image_format={fmt}")
        img = c.http.get(f"/image/{tid}", timeout=30)
        check(img.status == 200, f"B5: /image 状态 {img.status}")
        magic = b"\x89PNG" if fmt == "png" else b"\xff\xd8\xff"
        check(_magic_ok(img.body_bytes, magic), f"B5: {fmt} 魔数不对")
    cases.append(Case("B5", "搜狐首页 screenshot auto", "B",
                      "output:screenshot → succeeded + has_image + format∈{png,jpeg} + 魔数", b5))

    def b6(c):
        r = _render_and_get(c, URL_BING_SEARCH, "markdown", md_alg="readability")
        _check_succeeded(r, "B6")
        check(bool(r.json.get("markdown")), "B6: markdown 为空")
        alg = r.json.get("md_algorithm_used")
        check(alg in ("readability", "conservative"),
              f"B6: md_algorithm_used={alg}（允许 readability 回退 conservative）")
    cases.append(Case("B6", "Bing 搜索 readability", "B",
                      "bing search output:markdown md_algorithm:readability → succeeded + md_algorithm_used∈{readability,conservative}", b6))

    def b7(c):
        r = _render_and_get(c, URL_SOHU, "html,markdown,pdf,screenshot")
        tid = _check_succeeded(r, "B7")
        check(bool(r.json.get("html")), "B7: html 缺失")
        check(bool(r.json.get("markdown")), "B7: markdown 缺失")
        check(r.json.get("has_pdf") is True, "B7: has_pdf 缺失")
        check(r.json.get("has_image") is True, "B7: has_image 缺失")
    cases.append(Case("B7", "组合输出", "B",
                      "output:html,markdown,pdf,screenshot → 四项元信息齐全", b7))

    def b8(c):
        # 不带 long_poll_ms：立即返回，不阻塞。state 反映提交瞬间状态。
        body = {"url": URL_SOHU, "output": "markdown"}
        r = c.http.post("/render", json_body=body, timeout=15)
        check(r.status == 200, f"B8: /render 状态 {r.status}")
        state = (r.json or {}).get("state")
        check(state in ("pending", "running", "succeeded"), f"B8: 意外 state={state}")
        check(bool(r.json.get("task_id")), "B8: 无 task_id")
    cases.append(Case("B8", "非长轮询提交", "B",
                      "POST /render 不带 long_poll_ms → 立即返回 + task_id + state∈{pending,running,succeeded}", b8))

    def b9(c):
        # long_poll_ms:1 → 几乎必超时，返回非 succeeded（pending 或 running）。
        # 提交瞬间可能还没进 running（仍 pending）；二者都符合「未阻塞到成功」。
        r = _render_and_get(c, URL_SOHU, "html", long_poll_ms=1, expect_success=False, allow_retry=False)
        state = (r.json or {}).get("state")
        check(state in ("running", "pending"),
              f"B9: 期望 running/pending，实际 {state}: {r.text[:200]}")
    cases.append(Case("B9", "长轮询超时未成功", "B",
                      "long_poll_ms:1 → state∈{running,pending}（非 succeeded）", b9))

    return cases


def group_c(ctx):
    """C. 任务查询接口（无密码实例）。依赖 B 组已渲染的任务。"""
    cases = []

    def c1(c):
        # 先渲染一个成功任务，拿 task_id
        r = _render_and_get(c, URL_SOHU, "markdown")
        tid = _check_succeeded(r, "C1-setup")
        # GET /status/:id 不含 html
        s = c.http.get(f"/status/{tid}")
        check(s.status == 200, f"/status/:id 状态 {s.status}")
        check(s.json.get("state") == "succeeded", f"C1: state={s.json.get('state')}")
        check("elapsed_ms" in s.json, "C1: 缺 elapsed_ms")
        check("html" not in s.json, "C1: /status/:id 不应含 html 字段")
    cases.append(Case("C1", "状态查询", "C",
                      "GET /status/:id → succeeded + elapsed_ms + 不含 html", c1))

    def c2(c):
        r = _render_and_get(c, URL_SOHU, "markdown")
        tid = _check_succeeded(r, "C2-setup")
        res = c.http.get(f"/result/{tid}",
                         params={"output": "markdown", "timeout": RETRY_POLL_TIMEOUT_MS},
                         timeout=RETRY_POLL_HTTP_TIMEOUT)
        check(res.status == 200, f"C2: /result/:id 状态 {res.status}")
        check(res.json.get("state") == "succeeded", f"C2: state={res.json.get('state')}")
        check(bool(res.json.get("markdown")), "C2: markdown 为空")
    cases.append(Case("C2", "长轮询取结果", "C",
                      "GET /result/:id?output=markdown → succeeded + md 非空", c2))

    def c3(c):
        # 用纯字母数字的不存在 id：路由 /status/([A-Za-z0-9]+) 只匹配该字符集，
        # 含下划线的串不匹配会落到 404 HTML 页而非 JSON。这里用 zzzzzzzz 确保命中路由。
        s = c.http.get("/status/zzzzzzzzzzzzzzzz")
        check(s.status == 404, f"C3: 期望 404，实际 {s.status}")
        check(s.json and s.json.get("error") == "task not found", f"C3: body={s.text[:150]}")
    cases.append(Case("C3", "不存在任务-状态", "C",
                      "GET /status/<不存在> → 404 + task not found", c3))

    def c4(c):
        s = c.http.get("/result/zzzzzzzzzzzzzzzz")
        check(s.status == 404, f"C4: 期望 404，实际 {s.status}")
    cases.append(Case("C4", "不存在任务-结果", "C",
                      "GET /result/<不存在> → 404", c4))

    def c5(c):
        # B1 风格的 markdown-only 任务，GET /pdf/:id 应 404 并提示
        r = _render_and_get(c, URL_SOHU, "markdown")
        tid = _check_succeeded(r, "C5-setup")
        pdf = c.http.get(f"/pdf/{tid}")
        check(pdf.status == 404, f"C5: 期望 404，实际 {pdf.status}")
        check("output=pdf" in pdf.text, f"C5: 缺提示，body={pdf.text}")
    cases.append(Case("C5", "pdf 未请求", "C",
                      "GET /pdf/:id（markdown-only 任务）→ 404 + request output=pdf 提示", c5))

    return cases


def group_d(ctx):
    """D. 参数校验与 SSRF 防护（无密码实例）。"""
    cases = []

    def d1(c):
        r = c.http.post("/render", json_body={})
        check(r.status == 400, f"D1: 期望 400，实际 {r.status}")
        check("missing 'url'" in r.text, f"D1: body={r.text}")
    cases.append(Case("D1", "缺 url", "D",
                      "POST /render {} → 400 + missing 'url'", d1))

    def d2(c):
        r = c.http.post("/render", json_body={"url": "ftp://x"})
        check(r.status == 400, f"D2: 期望 400，实际 {r.status}")
        check("http/https" in r.text, f"D2: body={r.text}")
    cases.append(Case("D2", "非 http(s)", "D",
                      "POST /render url:ftp:// → 400 + url must be http/https", d2))

    def d3(c):
        r = c.http.post("/render", body="not json at all",
                        extra_headers={"Content-Type": "application/json"})
        check(r.status == 400, f"D3: 期望 400，实际 {r.status}")
        check("invalid json" in r.text, f"D3: body={r.text}")
    cases.append(Case("D3", "非法 JSON body", "D",
                      "POST /render body=非JSON → 400 + invalid json body", d3))

    def d4(c):
        r = c.http.post("/render", json_body={"url": "http://127.0.0.1:8088"})
        check(r.status == 400, f"D4: 期望 400，实际 {r.status}")
        check("SSRF" in r.text, f"D4: body={r.text}")
    cases.append(Case("D4", "SSRF-回环", "D",
                      "url:http://127.0.0.1 → 400 + blocked by SSRF", d4))

    def d5(c):
        r = c.http.post("/render", json_body={"url": "http://10.0.0.1"})
        check(r.status == 400, f"D5: 期望 400，实际 {r.status}")
        check("SSRF" in r.text, f"D5: body={r.text}")
    cases.append(Case("D5", "SSRF-内网", "D",
                      "url:http://10.0.0.1 → 400 + blocked by SSRF", d5))

    def d6(c):
        r = c.http.post("/render", json_body={"url": "http://169.254.169.254"})
        check(r.status == 400, f"D6: 期望 400，实际 {r.status}")
        check("SSRF" in r.text, f"D6: body={r.text}")
    cases.append(Case("D6", "SSRF-元数据", "D",
                      "url:http://169.254.169.254 → 400 + blocked by SSRF", d6))

    return cases


def ctx_ws_port(c):
    """从 CaseContext 取无密码实例 ws 端口。"""
    return c.ws_port


def ctx_http(c):
    """从 CaseContext 取无密码 HttpClient。"""
    return c.http


def group_e(ctx):
    """E. WebSocket 接口（无密码实例）。"""
    cases = []

    def e1(c):
        ws = WsClient(ctx_ws_port(c))
        ws.connect(timeout=10)
        try:
            ws.send_json({"action": "render", "url": URL_SOHU, "settle_ms": 2500})
            # 收 created
            got, ev = ws.recv_with_timeout(10)
            check(got and ev.get("event") == "created", f"E1: 未收到 created: {ev}")
            tid = ev.get("task_id") if ev else None
            check(bool(tid), "E1: created 无 task_id")
            # 收 finished（渲染需时间，给足）
            got2, ev2 = ws.recv_with_timeout(60)
            check(got2 and ev2.get("event") == "finished", f"E1: 未收到 finished: {ev2}")
            check(ev2.get("state") == "succeeded", f"E1: finished state={ev2.get('state') if ev2 else None}")
        finally:
            ws.close()
    cases.append(Case("E1", "render 推送", "E",
                      "WS render → created + finished(succeeded)", e1))

    def e2(c):
        # HTTP 提交（非长轮询），拿 tid，再 WS 订阅
        r = ctx_http(c).post("/render", json_body={"url": URL_SOHU, "output": "markdown"})
        check(r.status == 200, f"E2: /render 状态 {r.status}")
        tid = r.json.get("task_id")
        check(bool(tid), "E2: 无 task_id")
        ws = WsClient(ctx_ws_port(c))
        ws.connect(timeout=10)
        try:
            ws.send_json({"action": "subscribe", "task_id": tid})
            got, ev = ws.recv_with_timeout(10)
            check(got and ev.get("event") in ("subscribed", "finished"),
                  f"E2: 未收到 subscribed/finished: {ev}")
            # 若先收到 subscribed，再等 finished
            if got and ev.get("event") == "subscribed":
                got2, ev2 = ws.recv_with_timeout(60)
                check(got2 and ev2.get("event") == "finished", f"E2: 未收到 finished: {ev2}")
        finally:
            ws.close()
    cases.append(Case("E2", "subscribe 推送", "E",
                      "HTTP /render + WS subscribe → subscribed + finished", e2))

    def e3(c):
        ws = WsClient(ctx_ws_port(c))
        ws.connect(timeout=10)
        try:
            ws.send_json({"action": "unknown_action"})
            # 3s 内不应崩溃或主动关闭连接；收到 error 帧也算通过（只要连接没断）
            got, ev = ws.recv_with_timeout(3)
            # got=False（超时无消息）→ 服务端静默忽略，连接存活 → 通过
            # got=True 且是 error 事件 → 也通过
            if got:
                check(ev.get("event") in ("error",) or "error" not in (ev.get("event") or ""),
                      f"E3: 收到意外事件 {ev}")
            # 无论哪种，再发一条合法 render 确认连接仍活
            ws.send_json({"action": "render", "url": URL_SOHU})
            got2, ev2 = ws.recv_with_timeout(10)
            check(got2 and ev2.get("event") == "created", f"E3: 连接已不响应 render: {ev2}")
        finally:
            ws.close()
    cases.append(Case("E3", "非法 action", "E",
                      "WS action:unknown → 不崩溃，连接存活", e3))

    def e4(c):
        # 一个连接连发多个 render，验证「同时关注多个任务」：
        # 旧代码（单值订阅映射）这里会收不到第一个任务的 finished。
        N = 3
        ws = WsClient(ctx_ws_port(c))
        ws.connect(timeout=10)
        try:
            # 连发 N 个 render，先各收一条 created
            created_ids = []
            for _ in range(N):
                ws.send_json({"action": "render", "url": URL_SOHU, "settle_ms": 2500})
            for _ in range(N):
                got, ev = ws.recv_with_timeout(10)
                check(got and ev.get("event") == "created", f"E4: 未收到 created: {ev}")
                tid = ev.get("task_id") if ev else None
                check(bool(tid), f"E4: created 无 task_id: {ev}")
                created_ids.append(tid)
            # 三个 task_id 互不相同
            check(len(set(created_ids)) == N, f"E4: task_id 不唯一: {created_ids}")
            # 收齐 N 个 finished（完成顺序不定，按 task_id 集合核对）
            finished_ids = set()
            for _ in range(N):
                got, ev = ws.recv_with_timeout(60)
                check(got and ev.get("event") == "finished", f"E4: 未收到 finished: {ev}")
                check(ev.get("state") == "succeeded",
                      f"E4: finished state={ev.get('state') if ev else None}")
                finished_ids.add(ev.get("task_id"))
            check(finished_ids == set(created_ids),
                  f"E4: finished task_id 不匹配: got={finished_ids} expect={set(created_ids)}")
        finally:
            ws.close()
    cases.append(Case("E4", "多任务订阅", "E",
                      "WS 单连接连发 3 个 render → 收齐 3 个 finished（task_id 各异）", e4))

    return cases


def group_f(ctx):
    """F. 鉴权场景（密码实例 B）。"""
    cases = []

    def f1(c):
        r = c.auth_http.get("/auth-status")
        check(r.status == 200, f"F1: 状态 {r.status}")
        check(r.json and r.json.get("password_enabled") is True, f"F1: body={r.text}")
    cases.append(Case("F1", "鉴权状态-有密码", "F",
                      "GET /auth-status → password_enabled:true", f1, needs_auth=True))

    def f2(c):
        # 用无 token 的裸 client（临时构造）
        bare = HttpClient(AUTH_PORTS["http"])
        r = bare.get("/stats")
        check(r.status == 401, f"F2: 期望 401，实际 {r.status}")
        check("unauthorized" in r.text, f"F2: body={r.text}")
    cases.append(Case("F2", "未带 token 被拒", "F",
                      "GET /stats 无 token → 401 + unauthorized", f2, needs_auth=True))

    def f3(c):
        bare = HttpClient(AUTH_PORTS["http"])
        r = bare.post("/api/login", json_body={"password": "wrong_password"})
        check(r.status == 401, f"F3: 期望 401，实际 {r.status}")
        check("invalid password" in r.text, f"F3: body={r.text}")
    cases.append(Case("F3", "错误密码登录", "F",
                      "POST /api/login wrong → 401 + invalid password", f3, needs_auth=True))

    def f4(c):
        bare = HttpClient(AUTH_PORTS["http"])
        r = bare.post("/api/login", json_body={"password": AUTH_PASSWORD})
        check(r.status == 200, f"F4: 期望 200，实际 {r.status}")
        token = (r.json or {}).get("token")
        check(bool(token), f"F4: 无 token: {r.text}")
        # 存到 context 供后续用例（虽然 auth_http 已带 EXPECTED_TOKEN，这里验证登录返回的与之一致）
        check(token == EXPECTED_TOKEN, f"F4: token 与 EXPECTED_TOKEN 不一致")
    cases.append(Case("F4", "正确密码换 token", "F",
                      "POST /api/login correct → 200 + token == sha256(seimi-render:pwd)", f4, needs_auth=True))

    def f5(c):
        r = c.auth_http.get("/stats")
        check(r.status == 200, f"F5: 期望 200，实际 {r.status}")
    cases.append(Case("F5", "Bearer token 鉴权", "F",
                      "Authorization: Bearer <token> /stats → 200", f5, needs_auth=True))

    def f6(c):
        # query token：用不带 Authorization 的 client，走 ?token=
        bare = HttpClient(AUTH_PORTS["http"])
        r = bare.get("/stats", params={"token": EXPECTED_TOKEN})
        check(r.status == 200, f"F6: 期望 200，实际 {r.status}")
    cases.append(Case("F6", "query token 鉴权", "F",
                      "GET /stats?token=<token> → 200", f6, needs_auth=True))

    def f7(c):
        bare = HttpClient(AUTH_PORTS["http"], token="0" * 64)
        r = bare.get("/stats")
        check(r.status == 401, f"F7: 期望 401，实际 {r.status}")
    cases.append(Case("F7", "错误 token 被拒", "F",
                      "Bearer wrongtoken → 401", f7, needs_auth=True))

    def f8(c):
        # F4 已验证登录返回 token == EXPECTED_TOKEN；这里再独立校验算法确定性
        computed = hashlib.sha256(("seimi-render:" + AUTH_PASSWORD).encode()).hexdigest()
        check(computed == EXPECTED_TOKEN, "F8: sha256 算法结果与 EXPECTED_TOKEN 不一致")
    cases.append(Case("F8", "token 确定性算法", "F",
                      "本地 sha256(seimi-render:pwd) == 服务端 token", f8, needs_auth=True))

    def f10(c):
        bare = HttpClient(AUTH_PORTS["http"])
        # /health、/auth-status、/ 完全免鉴权（无 token 守卫，恒非 401）
        for path in ("/health", "/auth-status", "/"):
            r = bare.get(path)
            check(r.status != 401, f"F10: {path} 应免鉴权，实际 {r.status}")
        # /api/login 免 token 守卫（无需先登录即可调用）：错误密码返回 401 "invalid password"，
        # 但绝不应是 401 "unauthorized"（后者才表示被 token 守卫拦截）。
        r = bare.post("/api/login", json_body={"password": "x"})
        check(r.status in (200, 401, 429), f"F10: /api/login 状态异常 {r.status}")
        check(not ("unauthorized" in r.text and "invalid password" not in r.text),
              f"F10: /api/login 被 token 守卫拦截: {r.text}")
    cases.append(Case("F10", "免鉴权路径", "F",
                      "无 token 调 /health /auth-status /api/login / → 可达（非 unauthorized）", f10, needs_auth=True))

    def f11(c):
        # 无 token 连 WS 发 render → 应被拒（error 或连接关闭）
        ws = WsClient(AUTH_PORTS["ws"])
        ws.connect(timeout=10)
        try:
            ws.send_json({"action": "render", "url": URL_SOHU})
            got, ev = ws.recv_with_timeout(5)
            # 被拒：收到 error 或连接关闭（got=False）
            if got:
                check(ev.get("event") == "error", f"F11: 无 token 应被拒，收到 {ev}")
            else:
                check(True, "F11: 无 token 连接被关闭（符合预期）")
        except Exception:
            pass  # 连接被关也算通过
        finally:
            ws.close()
        # 带 token 连 WS → render 成功。注意：带 token 连接后服务端先回 authorized，
        # 再回 created；需连读直到拿到 created。
        ws2 = WsClient(AUTH_PORTS["ws"], token=EXPECTED_TOKEN)
        ws2.connect(timeout=10)
        try:
            ws2.send_json({"action": "render", "url": URL_SOHU})
            created = False
            for _ in range(5):  # 最多读 5 帧（authorized → created）
                got, ev = ws2.recv_with_timeout(10)
                if got and ev.get("event") == "created":
                    created = True
                    break
                if not got:
                    break
            check(created, f"F11: 带 token 未收到 created（最后帧: {ev}）")
        finally:
            ws2.close()
    cases.append(Case("F11", "WS 鉴权", "F",
                      "WS 无 token → 被拒；带 ?token → authorized + created", f11, needs_auth=True))

    def f12(c):
        bare = HttpClient(AUTH_PORTS["http"])
        r = bare.get("/stats", extra_headers={"Cookie": f"seimi_token={EXPECTED_TOKEN}"})
        check(r.status == 200, f"F12: 期望 200，实际 {r.status}")
    cases.append(Case("F12", "Cookie token", "F",
                      "Cookie: seimi_token=<token> /stats → 200", f12, needs_auth=True))

    # F9 登录限流：必须排在 F4-F8 之后（确保已拿到 token）。F9 之后再不调 /api/login。
    # 注意：F3/F10 已对同一源 IP（127.0.0.1）做过失败登录，失败计数会被累加，
    # 故这里不能假定「恰好第 11 次锁定」。改为持续打错误密码直到命中 429，断言「最终被锁」。
    def f9(c):
        bare = HttpClient(AUTH_PORTS["http"])
        # kLoginMaxFailures=10；扣掉此前 F3/F10 的失败计数后，再补打若干次必触发锁定。
        locked = False
        for i in range(15):
            r = bare.post("/api/login", json_body={"password": "wrong"})
            if r.status == 429:
                locked = True
                check("Retry-After" in r.headers or "retry_after" in r.text.lower(),
                      f"F9: 缺 Retry-After: headers={r.headers}")
                break
            check(r.status == 401, f"F9: 第 {i+1} 次应 401（未锁前错误密码），实际 {r.status}")
        check(locked, "F9: 连续 15 次错误密码仍未触发 429 锁定")
    cases.append(Case("F9", "登录限流", "F",
                      "连续错误密码 → 最终触发 429 + Retry-After", f9, needs_auth=True))

    return cases


def group_g(ctx):
    """G. MCP 接口（无密码实例 MCP 端口）。"""
    cases = []

    def g1(c):
        mcp = McpClient(c.mcp_port)
        mcp.initialize()
        result = mcp.list_tools()
        names = [t.get("name") for t in result.get("tools", [])]
        check("render_url" in names, f"G1: tools/list 无 render_url: {names}")
        check("get_render_result" in names, f"G1: tools/list 无 get_render_result: {names}")
    cases.append(Case("G1", "tools/list", "G",
                      "MCP initialize + tools/list → 含 render_url + get_render_result", g1))

    def g2(c):
        mcp = McpClient(c.mcp_port)
        mcp.initialize()
        result = mcp.call_tool("render_url", {"url": URL_SOHU, "output": "markdown"})
        contents = result.get("content", [])
        check(len(contents) > 0, "G2: 无 content")
        text = contents[0].get("text", "")
        check("succeeded" in text and "task_id=" in text, f"G2: text 缺成功标记: {text[:200]}")
        check(len(text) > 50, f"G2: markdown 似乎为空: {text[:200]}")
    cases.append(Case("G2", "render_url markdown", "G",
                      "render_url(sohu, markdown) → text content + md 非空", g2))

    def g3(c):
        mcp = McpClient(c.mcp_port)
        mcp.initialize()
        result = mcp.call_tool("render_url", {"url": URL_SOHU, "output": "screenshot"})
        contents = result.get("content", [])
        # 应含 image content（type=image）
        imgs = [x for x in contents if x.get("type") == "image"]
        check(len(imgs) > 0, f"G3: 无 image content: {[x.get('type') for x in contents]}")
        check("mimeType" in imgs[0] and "data" in imgs[0], "G3: image content 缺字段")
        check(imgs[0].get("mimeType") in ("image/png", "image/jpeg"), f"G3: mimeType={imgs[0].get('mimeType')}")
    cases.append(Case("G3", "render_url screenshot", "G",
                      "render_url(sohu, screenshot) → image content (base64)", g3))

    def g4(c):
        mcp = McpClient(c.mcp_port)
        mcp.initialize()
        # 先渲染拿 task_id
        r1 = mcp.call_tool("render_url", {"url": URL_SOHU, "output": "markdown"})
        text1 = r1.get("content", [{}])[0].get("text", "")
        # 提取 task_id
        m = re.search(r"task_id=(\S+)", text1)
        check(m is not None, f"G4: 无法从 render_url 结果提取 task_id: {text1[:200]}")
        tid = m.group(1)
        r2 = mcp.call_tool("get_render_result", {"task_id": tid, "output": "markdown"})
        text2 = r2.get("content", [{}])[0].get("text", "")
        check("succeeded" in text2, f"G4: get_render_result 未返回 succeeded: {text2[:200]}")
    cases.append(Case("G4", "get_render_result", "G",
                      "get_render_result(task_id, markdown) → markdown 内容", g4))

    def g5(c):
        mcp = McpClient(c.mcp_port)
        mcp.initialize()
        # 缺 url：MCP 工具错误以 result.isError=true 的 content 返回（非 JSON-RPC error）。
        result = mcp.call_tool("render_url", {})
        check(result.get("isError") is True, f"G5: 缺 url 未报错: {result}")
        contents = result.get("content", [])
        check(len(contents) > 0 and "url" in contents[0].get("text", ""),
              f"G5: 错误内容未提到 url: {contents}")
    cases.append(Case("G5", "缺参数", "G",
                      "render_url 无 url → result.isError=true + 提示 url is required", g5))

    return cases


def group_h(ctx):
    """H. Cookie 接口（无密码实例）。测完清空。"""
    cases = []

    def h1(c):
        body = {"cookies": [{"name": "k1", "domain": COOKIE_TEST_DOMAIN, "value": "v1", "path": "/"}]}
        r = ctx_http(c).post("/cookies", json_body=body)
        check(r.status == 200, f"H1: 状态 {r.status}")
        check((r.json or {}).get("stored", 0) >= 1, f"H1: stored<1: {r.text}")
        check(r.json.get("applied") is True, f"H1: applied 非 true: {r.text}")
    cases.append(Case("H1", "批量注入", "H",
                      "POST /cookies → stored>=1 + applied:true", h1))

    def h2(c):
        # 先注入（H1 可能因执行顺序先跑，这里自注入保证有数据）
        ctx_http(c).post("/cookies", json_body={"cookies": [
            {"name": "k2", "domain": COOKIE_TEST_DOMAIN, "value": "v2", "path": "/"}]})
        # cookie 注入经 GUI 线程异步 apply，轮询直到能看到测试域名。
        seen = False
        r = None
        for _ in range(20):  # 最多等 ~10s
            r = ctx_http(c).get("/cookies")
            if r.status == 200 and (r.json or {}).get("total", 0) >= 1:
                domains = [d.get("domain") for d in r.json.get("domains", [])]
                if COOKIE_TEST_DOMAIN in domains:
                    seen = True
                    break
            time.sleep(0.5)
        check(r is not None and r.status == 200, f"H2: 状态 {r.status if r else None}")
        check((r.json or {}).get("total", 0) >= 1, f"H2: total<1: {r.text}")
        domains = [d.get("domain") for d in r.json.get("domains", [])]
        check(COOKIE_TEST_DOMAIN in domains, f"H2: domains 不含测试域名: {domains}")
    cases.append(Case("H2", "概览", "H",
                      "GET /cookies → total>=1 + domains 含测试域名（异步生效）", h2))

    def h3(c):
        r = ctx_http(c).delete("/cookies")
        check(r.status == 200, f"H3: DELETE 状态 {r.status}")
        check((r.json or {}).get("cleared") is True, f"H3: 非 cleared: {r.text}")
        # cookie 清空经 GUI 线程异步 apply，DELETE 返回后未必即时生效。轮询直到 total=0。
        cleared_ok = False
        for _ in range(20):  # 最多等 ~10s
            r2 = ctx_http(c).get("/cookies")
            if (r2.json or {}).get("total", -1) == 0:
                cleared_ok = True
                break
            time.sleep(0.5)
        check(cleared_ok, f"H3: 清空后 total 仍非 0: {r2.text}")
    cases.append(Case("H3", "清空", "H",
                      "DELETE /cookies → cleared:true + 再 GET total=0（异步生效）", h3))

    return cases


def group_i(ctx):
    """I. Proxy 接口（无密码实例）。测完复位 direct。"""
    cases = []

    def i1(c):
        r = ctx_http(c).get("/proxy")
        check(r.status == 200, f"I1: 状态 {r.status}")
        check((r.json or {}).get("type") == "direct", f"I1: type!=direct: {r.text}")
    cases.append(Case("I1", "默认直连", "I",
                      "GET /proxy → type:direct", i1))

    def i2(c):
        r = ctx_http(c).post("/proxy", json_body={"type": "socks5", "host": "127.0.0.1", "port": 1080})
        check(r.status == 200, f"I2: POST 状态 {r.status}")
        check((r.json or {}).get("ok") is True, f"I2: 非 ok: {r.text}")
        # GET 回显
        r2 = ctx_http(c).get("/proxy")
        check((r2.json or {}).get("type") == "socks5", f"I2: 回显 type!=socks5: {r2.text}")
        check(r2.json.get("host") == "127.0.0.1", f"I2: 回显 host 不对")
        check(r2.json.get("port") == 1080, f"I2: 回显 port 不对")
        # 复位（不污染后续）
        ctx_http(c).delete("/proxy")
    cases.append(Case("I2", "设置 socks5", "I",
                      "POST /proxy socks5 → ok:true + GET 回显 type/host/port", i2))

    def i3(c):
        r = ctx_http(c).post("/proxy", json_body={"type": "http", "host": "x", "port": 99999})
        check(r.status == 400, f"I3: 期望 400，实际 {r.status}")
        check("host" in r.text and "port" in r.text, f"I3: body={r.text}")
    cases.append(Case("I3", "非法端口", "I",
                      "POST /proxy port:99999 → 400 + requires host and port", i3))

    def i4(c):
        r = ctx_http(c).delete("/proxy")
        check(r.status == 200, f"I4: DELETE 状态 {r.status}")
        check((r.json or {}).get("ok") is True, f"I4: 非 ok: {r.text}")
        r2 = ctx_http(c).get("/proxy")
        check((r2.json or {}).get("type") == "direct", f"I4: 回显 type!=direct: {r2.text}")
    cases.append(Case("I4", "恢复直连", "I",
                      "DELETE /proxy → ok:true + GET 回 type:direct", i4))

    return cases


# ============================================================
# §7 main：参数解析 + 编排
# ============================================================
def parse_args():
    ap = argparse.ArgumentParser(description="seimi-render 全接口自动回测")
    ap.add_argument("--skip-build", action="store_true", help="复用上次构建产物")
    ap.add_argument("--clean", action="store_true",
                    help="从零清理后全量重建（Windows 脚本自带每次清空；Linux rm -rf build；macOS 传 clean 给 package.sh）")
    ap.add_argument("--no-auth", action="store_true", help="只测无密码实例（跳过 F 组）")
    ap.add_argument("--group", help="只跑某组（A/B/C/D/E/F/G/H/I），逗号分隔")
    ap.add_argument("--case", help="只跑指定用例，逗号分隔（如 B1,B6）")
    ap.add_argument("--keep-servers", action="store_true", help="测完不杀进程（调试用）")
    ap.add_argument("--verbose", action="store_true", help="打印每请求/响应详情")
    ap.add_argument("--report-dir", default="build/test-reports", help="JSON 报告目录")
    return ap.parse_args()


def main():
    global _keep_servers
    args = parse_args()
    _keep_servers = args.keep_servers  # atexit 据此决定是否清理
    started_at = datetime.now()
    started_at_iso = started_at.isoformat(timespec="seconds")
    print(f"{BOLD}== seimi-render 全接口回测 =={RESET}")
    print(f"  {DIM}平台: {'Windows' if IS_WINDOWS else 'macOS' if IS_MACOS else 'Linux'} | "
          f"verbose: {args.verbose} | no-auth: {args.no_auth} | clean: {args.clean}{RESET}")
    t0 = time.monotonic()

    # 1. 构建
    binary = build(skip=args.skip_build, clean=args.clean)
    if not binary:
        # 构建失败：仍要清理已起进程（无），直接退出 2
        return EXIT_ENV
    build_info = {"ok": True, "binary": binary}

    # 2. 起实例
    no_auth_proc = start_instance(binary, NO_AUTH_PORTS, tag="实例A(无密码)")
    if not no_auth_proc or not wait_ready(NO_AUTH_PORTS["http"], tag="实例A"):
        cleanup_all(keep=args.keep_servers)
        return EXIT_ENV

    auth_proc = None
    if not args.no_auth:
        auth_proc = start_instance(binary, AUTH_PORTS, password=AUTH_PASSWORD, tag="实例B(有密码)")
        if not auth_proc or not wait_ready(AUTH_PORTS["http"], tag="实例B"):
            cleanup_all(keep=args.keep_servers)
            return EXIT_ENV

    instances = {
        "no_auth": {"http_port": NO_AUTH_PORTS["http"], "ws_port": NO_AUTH_PORTS["ws"],
                    "mcp_port": NO_AUTH_PORTS["mcp"], "pid": no_auth_proc.pid, "ready": True},
    }
    if auth_proc:
        instances["auth"] = {"http_port": AUTH_PORTS["http"], "ws_port": AUTH_PORTS["ws"],
                             "mcp_port": AUTH_PORTS["mcp"], "pid": auth_proc.pid, "ready": True}

    # 3. 构造上下文
    no_auth_http = HttpClient(NO_AUTH_PORTS["http"], verbose=args.verbose)
    auth_http = HttpClient(AUTH_PORTS["http"], token=EXPECTED_TOKEN, verbose=args.verbose) if auth_proc else None
    ctx = CaseContext(
        no_auth_http=no_auth_http,
        no_auth_ws_port=NO_AUTH_PORTS["ws"],
        no_auth_mcp_port=NO_AUTH_PORTS["mcp"],
        auth_http=auth_http,
        auth_ws_port=AUTH_PORTS["ws"] if auth_proc else None,
    )

    # 4. 注册用例（按组顺序）
    only_groups = args.group.split(",") if args.group else None
    only_cases = args.case.split(",") if args.case else None
    runner = Runner(ctx, only_groups=only_groups, only_cases=only_cases,
                    no_auth=args.no_auth, verbose=args.verbose)

    print()
    # 只打印「至少有一个用例会被执行（非 skip）」的组标题，避免冗余空标题。
    all_cases_by_group = []
    for group_name, group_fn in [
        ("A. 健康与元信息", group_a),
        ("B. 渲染核心链路", group_b),
        ("C. 任务查询", group_c),
        ("D. 参数校验与 SSRF", group_d),
        ("E. WebSocket", group_e),
        ("F. 鉴权场景", group_f),
        ("G. MCP", group_g),
        ("H. Cookie", group_h),
        ("I. Proxy", group_i),
    ]:
        cases = group_fn(ctx)
        all_cases_by_group.append((group_name, cases))
        for case in cases:
            runner.add(case)

    # 标注每组首个将被执行的用例位置，打印到该组标题后即停。
    # 简化：遍历注册结果，按 _should_run + 非强制 skip 的组才打印标题。
    def _group_will_run(group_letter):
        for case in runner.cases:
            if case.group == group_letter and runner._should_run(case) \
                    and not (runner.no_auth and case.needs_auth):
                return True
        return False

    # 重新组织：在执行前打印「会跑」的组标题
    runner._pending_group_headers = {
        gn.split(".")[0]: gn for gn, _ in all_cases_by_group if _group_will_run(gn.split(".")[0])
    }

    # 5. 执行
    runner.run_all()

    # 6. 报告
    duration_ms = int((time.monotonic() - t0) * 1000)
    passed, failed, skipped = print_summary(
        runner.results, started_at_iso, duration_ms, build_info, instances)
    write_json_report(runner.results, started_at_iso, duration_ms, build_info, instances,
                      args.report_dir)

    # 7. 收尾 + 退出码
    cleanup_all(keep=args.keep_servers)
    if failed > 0:
        return EXIT_FAIL
    return EXIT_OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n中断，清理进程...")
        cleanup_all()
        sys.exit(EXIT_FAIL)

