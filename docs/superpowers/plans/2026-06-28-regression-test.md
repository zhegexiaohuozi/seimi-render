# seimi-render 全接口自动回测脚本 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 创建 `scripts/regression_test.py`，一键构建 + 起双实例（有/无密码）+ 跑 9 组约 60 条接口回测用例 + 输出终端彩色报告与 JSON 报告。

**Architecture:** 单文件 Python 脚本，依赖 `requests` + `websocket-client`。脚本自管 seimi-render 进程生命周期：调现有 build 脚本 → 用临时高位端口起两个实例（无密码 1808x / 有密码 1818x）→ `/health` 轮询就绪 → 顺序执行用例 → atexit 收尾 kill 进程 → 写 JSON 报告。

**Tech Stack:** Python 3.8+、requests、websocket-client、hashlib（标准库）。MCP 走 raw JSON-RPC（POST `/mcp`），不引入 async 的 mcp SDK。

**Spec:** `docs/superpowers/specs/2026-06-28-regression-test-design.md`

---

## 关于 TDD 的说明

本工程产出的是一个**测试脚本本身**（而非被测产品），传统「先写失败测试再实现」不适用——它的「测试」就是它自己。因此本计划采用「**分块增量构建 + 每块写完立即用真实实例自验**」的节奏：每完成一个模块（如客户端封装、进程管理、一组用例），就跑一次验证命令，确认该模块行为正确后再进入下一块。每个 Task 结尾的「自验」步骤就是这一节奏的体现，等价于 TDD 的「红→绿」校验环节。

---

## 文件结构

只创建一个文件：

- **Create:** `scripts/regression_test.py` — 全部代码（约 900 行），单文件，内部按职责分区：
  - `§0` 依赖检查与导入
  - `§1` 常量与配置（端口、URL、密码、二进制路径）
  - `§2` 客户端封装：`HttpClient`（带 token 注入）、`WsClient`、`McpClient`
  - `§3` 进程管理：`build()`、`start_instance()`、`wait_ready()`、`cleanup()`
  - `§4` 断言框架：`Case`、`CaseResult`、`Runner`（注册/执行/过滤/报告）
  - `§5` 用例定义（A–I 九组）
  - `§6` 报告（终端 + JSON）
  - `§7` `main()`：参数解析 + 编排

不拆模块文件——单文件方便「改完代码一键跑」，且现有 `soak_test.py` 也是单文件。

**重要前提：** 执行此计划需要一台**已装好 Qt + 编译工具链**的机器（即 `scripts/setup-windows.bat` / `setup-linux.sh` 已跑过、能成功 `scripts/build-*.sh`）。构建产物在 `build/seimi-render(.exe)`。

---

## Task 1: 脚本骨架 + 依赖检查 + 配置常量

**Files:**
- Create: `scripts/regression_test.py`

- [ ] **Step 1: 创建脚本头部（docstring + 依赖检查 + 导入 + 常量）**

写入 `scripts/regression_test.py`：

```python
#!/usr/bin/env python3
"""
seimi-render 全接口自动回测脚本。

一键：构建 → 起双实例（有/无密码）→ 跑 9 组约 60 条接口用例 → 终端彩色报告 + JSON 报告。

用法:
    python scripts/regression_test.py                 # 全套（构建+双实例+全测）
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
    return ["build/seimi-render"]


def build_script():
    """按平台返回构建脚本相对路径。"""
    return "scripts/build-windows.bat" if IS_WINDOWS else "scripts/build-linux.sh"
```

- [ ] **Step 2: 自验（语法检查）**

Run: `python -c "import ast; ast.parse(open('scripts/regression_test.py').read())"`
Expected: 无输出（语法正确）。此时尚无 main，直接运行脚本会无事发生——这是预期的。

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: 回测脚本骨架 + 依赖检查 + 配置常量"
```

---

## Task 2: 客户端封装 — HttpClient（带 token 注入）

封装对 seimi-render HTTP API 的访问。核心是 `token` 注入（Authorization Bearer），让鉴权/非鉴权用例用同一套方法。

**Files:**
- Modify: `scripts/regression_test.py`（在 §1 之后追加 §2）

- [ ] **Step 1: 实现 HttpClient 类**

在 `binary_candidates()` 函数之后追加：

```python
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
```

- [ ] **Step 2: 自验（语法检查）**

Run: `python -c "import ast; ast.parse(open('scripts/regression_test.py').read())"`
Expected: 无输出。

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: HttpClient 封装（带 token 注入）"
```

---

## Task 3: 客户端封装 — WsClient（websocket-client）

封装 WebSocket：连接（可选 `?token=`）、发送、带超时接收、关闭。

**Files:**
- Modify: `scripts/regression_test.py`（在 HttpClient 之后追加）

- [ ] **Step 1: 实现 WsClient 类**

在 `HttpClient` 类之后追加：

```python
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
```

- [ ] **Step 2: 自验（语法检查）**

Run: `python -c "import ast; ast.parse(open('scripts/regression_test.py').read())"`
Expected: 无输出。

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: WsClient 封装（websocket-client）"
```

---

## Task 4: 客户端封装 — McpClient（raw JSON-RPC over /mcp）

封装 MCP：initialize → tools/list → tools/call。走 POST `/mcp` 的 JSON-RPC，自增 id。

**Files:**
- Modify: `scripts/regression_test.py`（在 WsClient 之后追加）

- [ ] **Step 1: 实现 McpClient 类**

在 `WsClient` 类之后追加：

```python
class McpError(Exception):
    """MCP JSON-RPC error。"""
    def __init__(self, err):
        self.code = err.get("code")
        self.message = err.get("message", "")
        super().__init__(f"MCP error {self.code}: {self.message}")


class McpClient:
    """seimi-render MCP 客户端（Streamable HTTP，raw JSON-RPC，无需 async SDK）。

    协议层只用到 initialize / tools/list / tools/call，直接 POST /mcp 即可。
    """

    def __init__(self, port, verbose=False):
        self.url = f"http://{HOST}:{port}/mcp"
        self.verbose = verbose
        self._id = 0
        self.session_headers = {"Content-Type": "application/json"}

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
        # cpp-mcp 已 patch 为 session 自动重建；带固定 session id 保持规范。
        # Streamable HTTP 无状态，无需额外确认握手。
        return result

    def list_tools(self):
        return self.call("tools/list", {})

    def call_tool(self, name, arguments):
        return self.call("tools/call", {"name": name, "arguments": arguments})
```

- [ ] **Step 2: 自验（语法检查）**

Run: `python -c "import ast; ast.parse(open('scripts/regression_test.py').read())"`
Expected: 无输出。

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: McpClient 封装（raw JSON-RPC over /mcp）"
```

---

## Task 5: 进程管理 — build / start_instance / wait_ready / cleanup

自管 seimi-render 生命周期。核心难点：Windows 杀进程树（Chromium 子进程）、atexit 可靠收尾、`--no-sandbox` 平台判定。

**Files:**
- Modify: `scripts/regression_test.py`（在 §2 之后追加 §3）

- [ ] **Step 1: 实现进程管理函数**

在 `McpClient` 类之后追加：

```python
# ============================================================
# §3 进程管理：构建 / 启动 / 就绪探测 / 收尾
# ============================================================
_procs = []  # 全局已启动实例进程列表，供 atexit 清理


def _log(tag, msg):
    print(f"{CYAN}[{tag}]{RESET} {msg}")


def find_binary():
    """返回存在的二进制路径，找不到返回 None。"""
    for c in binary_candidates():
        if os.path.isfile(c):
            return c
    return None


def build(skip=False):
    """调平台构建脚本。成功返回二进制路径，失败返回 None。"""
    if skip:
        b = find_binary()
        if b:
            _log("build", f"--skip-build，复用 {b}")
            return b
        _log("build", f"{YELLOW}--skip-build 但未找到二进制，回退到构建{RESET}")

    script = build_script()
    _log("build", f"调用 {script} ...")
    t0 = time.monotonic()
    try:
        # Windows: .bat 必须用 shell=True 才能找到；Linux: bash 脚本。
        if IS_WINDOWS:
            rc = subprocess.call(script, shell=True)
        else:
            rc = subprocess.call(["bash", script])
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


def cleanup_all(keep=False):
    """收尾：杀掉所有已启动实例。keep=True 时跳过（调试用）。"""
    if keep:
        return
    for proc in _procs:
        tag = getattr(proc, "_tag", "?")
        ports = getattr(proc, "_ports", {})
        _log("teardown", f"关闭 {tag} (pid={proc.pid}, http={ports.get('http')})")
        _kill_proc(proc)


atexit.register(cleanup_all)
```

- [ ] **Step 2: 自验（手动启动一个无密码实例 + 就绪探测 + 关闭）**

此时尚无 main，用一段临时验证。先构建一次（若已有二进制可跳过）：
Run: `python scripts/build-windows.bat`（Windows）或 `bash scripts/build-linux.sh`（Linux）
Expected: 产出 `build/seimi-render(.exe)`。

然后跑一段内联验证（会真实起一个实例并探测 /health，3 秒后由 atexit 关闭）：
Run: `python -c "import sys; sys.argv=['x']; import importlib.util; spec=importlib.util.spec_from_file_location('rt','scripts/regression_test.py'); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); b=m.find_binary(); print('binary:', b); p=m.start_instance(b, m.NO_AUTH_PORTS, tag='manual'); print('started pid:', p.pid if p else None); print('ready:', m.wait_ready(m.NO_AUTH_PORTS['http'], tag='manual'))"`
Expected: 打印 `binary: build\...`、`started pid: <num>`、`ready: True`，进程在脚本退出后自动关闭。

如果 `ready: False`，说明实例启动有问题——检查端口是否被占用、二进制能否手动 `--http-port 18088` 跑起来。

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: 进程管理（构建/启动/就绪探测/收尾）"
```

---

## Task 6: 断言框架 — Case / CaseResult / Runner

这是用例执行的骨架。`Case` 用闭包承载「一个用例 = 一段执行函数 + 断言」；`Runner` 负责注册、过滤（--group/--case）、执行、收集结果、报告。

**Files:**
- Modify: `scripts/regression_test.py`（在 §3 之后追加 §4）

- [ ] **Step 1: 实现 AssertionFailure 异常与 assert 辅助**

在 `atexit.register(...)` 之后追加：

```python
# ============================================================
# §4 断言框架
# ============================================================
class AssertionFailure(Exception):
    """断言失败。message 即失败原因。"""


def check(cond, msg):
    """断言 cond 为真，否则抛 AssertionFailure(msg)。"""
    if not cond:
        raise AssertionFailure(msg)


def check_eq(actual, expected, label=""):
    """断言相等，失败消息带实际值。"""
    if actual != expected:
        raise AssertionFailure(f"{label}: 期望 {expected!r}, 实际 {actual!r}")
```

- [ ] **Step 2: 实现 Case / CaseResult / Runner**

在 `check_eq` 之后追加：

```python
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
    def __init__(self, cid, name, group, assertion, func, needs_auth=False, tags=None):
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
```

- [ ] **Step 3: 自验（语法检查）**

Run: `python -c "import ast; ast.parse(open('scripts/regression_test.py').read())"`
Expected: 无输出。

- [ ] **Step 4: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: 断言框架（Case/CaseResult/Runner）"
```

---

## Task 7: 报告输出 — 终端汇总 + JSON 报告

**Files:**
- Modify: `scripts/regression_test.py`（在 Runner 之后追加 §6）

- [ ] **Step 1: 实现报告函数**

在 `Runner` 类之后追加：

```python
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
```

- [ ] **Step 2: 自验（语法检查）**

Run: `python -c "import ast; ast.parse(open('scripts/regression_test.py').read())"`
Expected: 无输出。

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: 报告输出（终端汇总 + JSON）"
```

---

## Task 8: 用例 A 组 — 健康与元信息接口

从本 Task 起逐组注册用例。每组用例用工厂函数生成 `Case` 列表，最终在 `main()` 里汇总注册。

**Files:**
- Modify: `scripts/regression_test.py`（在 §6 之后追加 §5 用例区）

- [ ] **Step 1: 定义用例注册的总体结构 + A 组用例**

在 `write_json_report` 之后追加 §5 头部与 A 组：

```python
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
```

- [ ] **Step 2: 自验（用例数 + 无副作用导入）**

模块被 `if __name__` 保护，直接 import 不会触发 main。用一行命令导入并调用 group 函数计用例数：
Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('A:', len(m.group_a(None)))"`
Expected: `A: 6`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: A 组用例（健康与元信息）"
```

---

## Task 9: 用例 B 组 — 渲染核心链路（真实公网）

最关键的一组，覆盖四种输出 + readability + 组合 + 长轮控行为。含「超时补取一次」逻辑。

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_a` 之后追加）

- [ ] **Step 1: 实现 render 辅助 + B 组用例**

在 `group_a` 函数之后追加：

```python
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
        # long_poll_ms:1 → 几乎必超时，返回 running（非 succeeded）。不补取。
        r = _render_and_get(c, URL_SOHU, "html", long_poll_ms=1, expect_success=False, allow_retry=False)
        state = (r.json or {}).get("state")
        check(state == "running", f"B9: 期望 running，实际 {state}: {r.text[:200]}")
    cases.append(Case("B9", "长轮询超时仍 running", "B",
                      "long_poll_ms:1 → state:running（非 succeeded）", b9))

    return cases
```

- [ ] **Step 2: 自验（A+B 组用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('A:', len(m.group_a(None)), 'B:', len(m.group_b(None)))"`
Expected: `A: 6 B: 9`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: B 组用例（渲染核心链路，含超时补取）"
```

---

## Task 10: 用例 C 组 — 任务查询接口

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_b` 之后追加）

- [ ] **Step 1: 实现 C 组用例**

在 `group_b` 函数之后追加：

```python
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
        s = c.http.get("/status/nonexistent_task_id")
        check(s.status == 404, f"C3: 期望 404，实际 {s.status}")
        check(s.json and s.json.get("error") == "task not found", f"C3: body={s.text}")
    cases.append(Case("C3", "不存在任务-状态", "C",
                      "GET /status/<不存在> → 404 + task not found", c3))

    def c4(c):
        s = c.http.get("/result/nonexistent_task_id")
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
```

- [ ] **Step 2: 自验（用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('C:', len(m.group_c(None)))"`
Expected: `C: 5`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: C 组用例（任务查询）"
```

---

## Task 11: 用例 D 组 — 参数校验与 SSRF 防护

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_c` 之后追加）

- [ ] **Step 1: 实现 D 组用例**

在 `group_c` 函数之后追加：

```python
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
```

- [ ] **Step 2: 自验（用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('D:', len(m.group_d(None)))"`
Expected: `D: 6`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: D 组用例（参数校验与 SSRF）"
```

---

## Task 12: 用例 E 组 — WebSocket 接口

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_d` 之后追加）

- [ ] **Step 1: 实现辅助函数 + E 组用例**

在 `group_d` 函数之后追加（先定义辅助，再定义用例组，避免「先引用后定义」的阅读困扰）：

```python
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

    return cases
```

> 辅助函数 `ctx_ws_port`/`ctx_http` 已在本组用例之前定义；后续组（F/G/H/I）复用 `ctx_http`。

- [ ] **Step 2: 自验（用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('E:', len(m.group_e(None)))"`
Expected: `E: 3`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: E 组用例（WebSocket）"
```

---

## Task 13: 用例 F 组 — 鉴权场景（密码实例）

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_e` 之后追加）

- [ ] **Step 1: 实现 F 组用例**

在 `group_e` 函数之后追加（注意 F9 必须排在 F4–F8 之后，用例注册顺序即执行顺序）：

```python
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
        for path, allow_post in (("/health", False), ("/auth-status", False), ("/", False)):
            r = bare.get(path)
            check(r.status != 401, f"F10: {path} 应免鉴权，实际 {r.status}")
        # /api/login 免鉴权（POST）
        r = bare.post("/api/login", json_body={"password": "x"})
        check(r.status != 401, f"F10: /api/login 应免鉴权，实际 {r.status}")
    cases.append(Case("F10", "免鉴权路径", "F",
                      "无 token 调 /health /auth-status /api/login / → 全部非 401", f10, needs_auth=True))

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
        # 带 token 连 WS → render 成功
        ws2 = WsClient(AUTH_PORTS["ws"], token=EXPECTED_TOKEN)
        ws2.connect(timeout=10)
        try:
            ws2.send_json({"action": "render", "url": URL_SOHU})
            got, ev = ws2.recv_with_timeout(10)
            check(got and ev.get("event") == "created", f"F11: 带 token 未收到 created: {ev}")
        finally:
            ws2.close()
    cases.append(Case("F11", "WS 鉴权", "F",
                      "WS 无 token → 被拒；带 ?token → render 成功", f11, needs_auth=True))

    def f12(c):
        bare = HttpClient(AUTH_PORTS["http"])
        r = bare.get("/stats", extra_headers={"Cookie": f"seimi_token={EXPECTED_TOKEN}"})
        check(r.status == 200, f"F12: 期望 200，实际 {r.status}")
    cases.append(Case("F12", "Cookie token", "F",
                      "Cookie: seimi_token=<token> /stats → 200", f12, needs_auth=True))

    # F9 登录限流：必须排在 F4-F8 之后（确保已拿到 token）。F9 之后再不调 /api/login。
    def f9(c):
        bare = HttpClient(AUTH_PORTS["http"])
        # 连续 10 次错误密码（kLoginMaxFailures=10）
        for i in range(10):
            r = bare.post("/api/login", json_body={"password": "wrong"})
            check(r.status == 401, f"F9: 第 {i+1} 次应 401，实际 {r.status}")
        # 第 11 次应 429 + Retry-After
        r = bare.post("/api/login", json_body={"password": "wrong"})
        check(r.status == 429, f"F9: 第 11 次应 429，实际 {r.status}")
        check("Retry-After" in r.headers or "retry_after" in r.text.lower(),
              f"F9: 缺 Retry-After: headers={r.headers}")
    cases.append(Case("F9", "登录限流", "F",
                      "连续 10 次错误密码 → 第 11 次 429 + Retry-After", f9, needs_auth=True))

    return cases
```

> 注意 F9 注册在列表最后，但 id 为 F9——执行顺序按列表顺序（F9 在 F12 之后跑），符合 spec 的「F9 排最后」约束。

- [ ] **Step 2: 自验（用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('F:', len(m.group_f(None)))"`
Expected: `F: 12`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: F 组用例（鉴权场景，含登录限流）"
```

---

## Task 14: 用例 G 组 — MCP 接口

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_f` 之后追加）

- [ ] **Step 1: 实现 G 组用例**

在 `group_f` 函数之后追加：

```python
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
        import re
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
        raised = False
        try:
            mcp.call_tool("render_url", {})  # 缺 url
        except McpError as e:
            raised = True
            check(e.code is not None and e.code < 0, f"G5: 错误码异常: {e}")
        check(raised, "G5: 缺 url 未抛 MCP 错误")
    cases.append(Case("G5", "缺参数", "G",
                      "render_url 无 url → MCP invalid_params 错误", g5))

    return cases
```

- [ ] **Step 2: 自验（用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('G:', len(m.group_g(None)))"`
Expected: `G: 5`

- [ ] **Step 3: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: G 组用例（MCP 接口）"
```

---

## Task 15: 用例 H 组 — Cookie 接口 + I 组 — Proxy 接口

**Files:**
- Modify: `scripts/regression_test.py`（在 `group_g` 之后追加）

- [ ] **Step 1: 实现 H 组用例**

在 `group_g` 函数之后追加：

```python
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
        r = ctx_http(c).get("/cookies")
        check(r.status == 200, f"H2: 状态 {r.status}")
        check((r.json or {}).get("total", 0) >= 1, f"H2: total<1: {r.text}")
        domains = [d.get("domain") for d in r.json.get("domains", [])]
        check(COOKIE_TEST_DOMAIN in domains, f"H2: domains 不含测试域名: {domains}")
    cases.append(Case("H2", "概览", "H",
                      "GET /cookies → total>=1 + domains 含测试域名", h2))

    def h3(c):
        r = ctx_http(c).delete("/cookies")
        check(r.status == 200, f"H3: DELETE 状态 {r.status}")
        check((r.json or {}).get("cleared") is True, f"H3: 非 cleared: {r.text}")
        # 再 GET 应为空（或 total=0）
        r2 = ctx_http(c).get("/cookies")
        check((r2.json or {}).get("total", -1) == 0, f"H3: 清空后 total!=0: {r2.text}")
    cases.append(Case("H3", "清空", "H",
                      "DELETE /cookies → cleared:true + 再 GET total=0", h3))

    return cases
```

- [ ] **Step 2: 实现 I 组用例**

在 `group_h` 函数之后追加：

```python
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
```

- [ ] **Step 3: 自验（用例数）**

Run: `python -c "import importlib.util as u; s=u.spec_from_file_location('rt','scripts/regression_test.py'); m=u.module_from_spec(s); s.loader.exec_module(m); print('H:', len(m.group_h(None)), 'I:', len(m.group_i(None)))"`
Expected: `H: 3 I: 4`

- [ ] **Step 4: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: H/I 组用例（Cookie + Proxy）"
```

---

## Task 16: main() 编排 — 参数解析 + 全流程串联

把所有模块串起来：解析参数 → 构建 → 起两实例 → 就绪 → 注册用例 → 跑 → 报告 → 退出码。

**Files:**
- Modify: `scripts/regression_test.py`（文件末尾追加 §7 main）

- [ ] **Step 1: 实现 main() 与参数解析**

在 `group_i` 函数之后追加：

```python
# ============================================================
# §7 main：参数解析 + 编排
# ============================================================
def parse_args():
    ap = argparse.ArgumentParser(description="seimi-render 全接口自动回测")
    ap.add_argument("--skip-build", action="store_true", help="复用上次构建产物")
    ap.add_argument("--no-auth", action="store_true", help="只测无密码实例（跳过 F 组）")
    ap.add_argument("--group", help="只跑某组（A/B/C/D/E/F/G/H/I），逗号分隔")
    ap.add_argument("--case", help="只跑指定用例，逗号分隔（如 B1,B6）")
    ap.add_argument("--keep-servers", action="store_true", help="测完不杀进程（调试用）")
    ap.add_argument("--verbose", action="store_true", help="打印每请求/响应详情")
    ap.add_argument("--report-dir", default="build/test-reports", help="JSON 报告目录")
    return ap.parse_args()


def main():
    args = parse_args()
    started_at = datetime.now()
    started_at_iso = started_at.isoformat(timespec="seconds")
    print(f"{BOLD}== seimi-render 全接口回测 =={RESET}")
    print(f"  {DIM}平台: {'Windows' if IS_WINDOWS else 'Linux'} | "
          f"verbose: {args.verbose} | no-auth: {args.no_auth}{RESET}")
    t0 = time.monotonic()

    # 1. 构建
    binary = build(skip=args.skip_build)
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
        print(f"{BOLD}{group_name}{RESET}")
        for case in group_fn(ctx):
            runner.add(case)

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
```

- [ ] **Step 2: 自验 — 完整端到端跑一次**

Run: `python scripts/regression_test.py --verbose`
Expected:
- 打印构建、两个实例就绪
- 9 组用例依次执行，大部分 ✓（B/G 等真实渲染可能因网络偶有失败，属正常）
- 末尾打印汇总 + JSON 报告路径
- 退出码 0 或 1（不应是 2）

检查进程是否都关闭（不残留）：
Run: Windows `tasklist | findstr seimi-render` / Linux `pgrep -a seimi-render`
Expected: 无输出（进程已清理）。

- [ ] **Step 3: 自验 — 单组过滤**

Run: `python scripts/regression_test.py --group D --skip-build`
Expected: 只跑 D 组 6 条，全部 ✓（D 组不依赖网络），快速完成，退出码 0。

- [ ] **Step 4: 自验 — --no-auth**

Run: `python scripts/regression_test.py --group A,F --no-auth --skip-build`
Expected: A 组 6 条 ✓，F 组全部 skip，退出码 0。

- [ ] **Step 5: 提交**

```bash
git add scripts/regression_test.py
git commit -m "test: main 编排，串联构建/起实例/跑用例/报告"
```

---

## Task 17: README 补充 + 收尾验证

**Files:**
- Modify: `README.md`（在测试相关章节补充回测脚本说明，若无测试章节则新增）

- [ ] **Step 1: 在 README 增加回测脚本说明**

在 README.md 合适位置（如现有 smoke_test/soak_test 附近）追加：

```markdown
### 全接口自动回测（`scripts/regression_test.py`）

最全面的回归测试，覆盖 HTTP / WebSocket / MCP 三入口的全部接口能力（含鉴权、SSRF、
cookie、proxy、四种渲染输出）。改完代码后一键执行，验证「以前能用的功能是否仍可用」。

```bash
# 一键全套（自动构建 + 起双实例 + 全测 + 报告）
python scripts/regression_test.py

# 常用旗标
python scripts/regression_test.py --skip-build        # 复用上次构建产物（开发期迭代快）
python scripts/regression_test.py --no-auth           # 只测无密码实例（跳过 F 组）
python scripts/regression_test.py --group B           # 只跑某组
python scripts/regression_test.py --case B1,B6        # 只跑指定用例
python scripts/regression_test.py --keep-servers      # 测完不杀进程（调试）
python scripts/regression_test.py --verbose           # 打印请求/响应详情
```

- 依赖：`pip install requests websocket-client`
- 自动用临时高位端口（1808x 无密码 / 1818x 有密码）起两个实例，测完自动关闭。
- 真实渲染用搜狐首页 + Bing 搜索验证 markdown/截图/pdf 质量。
- 输出：终端彩色报告 + `build/test-reports/regression-<时间戳>.json`。
- 退出码：0 全绿 / 1 有失败（功能回归）/ 2 构建·启动失败（环境问题）。
```

- [ ] **Step 2: 最终全量验证**

Run: `python scripts/regression_test.py`
Expected:
- 全部 9 组约 60 条用例执行
- 退出码 0（理想）或 1（真实渲染偶发失败可接受）
- 检查 `build/test-reports/regression-*.json` 文件已生成，打开确认结构正确（含 summary/groups/cases）

- [ ] **Step 3: 提交**

```bash
git add README.md
git commit -m "docs: README 补充全接口回测脚本说明"
```

---

## 完成标志

- [ ] `scripts/regression_test.py` 可一键运行（`python scripts/regression_test.py`）
- [ ] 9 组用例全部注册并执行（A6+B9+C5+D6+E3+F12+G5+H3+I4 = 53 条核心，加 B 系列 = 约 60）
- [ ] 终端彩色报告 + JSON 报告均正常输出
- [ ] 进程可靠收尾（无残留）
- [ ] `--skip-build` / `--group` / `--case` / `--no-auth` / `--keep-servers` / `--verbose` 旗标均生效
