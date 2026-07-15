# seimi-render 全接口自动回测脚本 — 设计文档

- 日期：2026-06-28
- 状态：待 review
- 落点：`scripts/regression_test.py`（新增，单文件）

## 1. 背景与目标

seimi-render 对外提供 HTTP / WebSocket / MCP 三种接入方式，覆盖渲染（html / markdown / pdf / screenshot 四种输出）、鉴权（密码 → 确定性 token、登录限流）、SSRF 防护、cookie 注入、网络代理动态配置等能力。现有测试只有 `scripts/smoke_test.sh`（5 条基础冒烟）与 `scripts/soak_test.py`（压测），**远未覆盖全量接口行为**。

目标：写一个**全面的自动回测脚本**，每次代码改动后一键执行，回归验证「以前能用的功能是否仍可用」。具体要求（来自澄清）：

1. **服务生命周期全自动**：脚本自管「构建 → 启动 → 测试 → 关闭」。
2. **同时覆盖有密码与无密码两种鉴权场景**（因此必须自起两个实例）。
3. **真实渲染用 bing 搜索 + 搜狐首页**（内容足够丰富，能验证 markdown/截图质量）。
4. **结果同时输出到终端（彩色）和 JSON 报告文件**。
5. 构建环节复用现有 `scripts/build-windows.bat` / `scripts/build-linux.sh`。

## 2. 总体架构

单一 Python 脚本，**依赖少量常见第三方包**（`requests` 做 HTTP、`websocket-client` 做 WebSocket），其余用标准库。MCP 协议层只用到 `initialize`/`tools/list`/`tools/call`，直接用 `requests` 发 JSON-RPC 最简洁，不引入 async 的官方 `mcp` SDK。依赖在脚本头部声明，运行前需 `pip install requests websocket-client`。

```
main()
  ├─ 1. 构建：按平台调 scripts/build-windows.bat 或 scripts/build-linux.sh
  ├─ 2. 定位二进制（build/seimi-render(.exe) 或 Release 子目录）
  ├─ 3. 起两个实例（临时高位端口，进程隔离）：
  │      • 实例 A 无密码：--http-port 18088 --ws-port 18089 --mcp-port 18090
  │      • 实例 B 有密码：--http-port 18188 --ws-port 18189 --mcp-port 18190
  │                        --password testpass_2024
  │      （Linux/root/WSL 加 --no-sandbox）
  ├─ 4. 用 GET /health 轮询，等两个实例就绪（最长 60s）
  ├─ 5. 顺序执行 9 组测试用例（见第 4 节）
  ├─ 6. 收尾：kill 两个进程 + 写 JSON 报告
  └─ 7. exit code：0 全绿 / 1 有失败 / 2 构建·启动失败
```

### 2.1 为什么自起两个实例

要同时覆盖「无密码」与「有密码」两种鉴权配置，必须同时运行两个配置不同的进程。手动启停做不到这一点（用户得自己开两个终端、记住两组端口、跑完手动关），所以服务生命周期必须由脚本自管。这也带来一个额外好处：测试中可放心做「破坏性」验证（如登录限流会锁 IP 10 分钟），因为实例是临时的，测完即销毁，不污染任何外部状态。

### 2.2 平台自适应

| 维度 | Windows | Linux/macOS |
|------|---------|-------------|
| 构建脚本 | `scripts/build-windows.bat` | `scripts/build-linux.sh` |
| 二进制候选路径 | `build/seimi-render.exe`、`build/Release/seimi-render.exe` | `build/seimi-render` |
| 子进程组 | `creationflags=CREATE_NEW_PROCESS_GROUP` | 默认进程组 |
| 终止进程 | `taskkill /F /T /PID`（杀整树，含 Chromium 子进程） | `SIGTERM`，5s 超时后 `SIGKILL` 兜底 |
| `--no-sandbox` | 不需要 | root/WSL/容器需要（脚本检测 root 时自动加） |

## 3. 关键技术实现细节

### 3.1 进程管理

**启动参数（关键）：**

```
实例 A（无密码）：
  seimi-render --http-port 18088 --ws-port 18089 --mcp-port 18090
               --host 127.0.0.1 [--no-sandbox]

实例 B（有密码）：
  seimi-render --http-port 18188 --ws-port 18189 --mcp-port 18190
               --host 127.0.0.1 --password testpass_2024 [--no-sandbox]
```

实例 B 也启用 MCP 端口（保持完整），但测试不在 B 上测 MCP 鉴权（MCP token 与 HTTP 同源，已被 F 系列覆盖）。

**可靠收尾（防泄漏）：**
- `atexit` 注册清理钩子，无论正常退出 / 异常 / Ctrl-C 都尝试 kill 两个进程。
- 记录每个子进程 PID，用 PID 精确 kill。
- kill 后 `proc.wait(timeout=5)` 确认退出；超时强杀。
- Windows 用 `taskkill /F /T /PID` 杀整棵进程树（Chromium 会拉起子进程，单杀父进程会残留子进程占用端口）。

### 3.2 实例就绪探测

二进制启动后需要初始化 Qt + WebEngine，不能假设立即就绪：

```python
def wait_ready(http_port, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = GET http://127.0.0.1:{http_port}/health   # /health 始终免鉴权
            if resp.status == 200 and "ok" in body: return True
        except ConnectionError:
            time.sleep(1)
    raise RuntimeError("实例 60s 未就绪")
```

用 `/health`（免鉴权）探测，不用 `/`（有密码实例返回登录页 HTML，难判）。

### 3.3 WebSocket（用 websocket-client）

用 `websocket-client`（`import websocket`）替代手写帧编解码，连接、收发、超时控制都现成：

- 连接：`websocket.create_connection(url, timeout=N)`，url 形如 `ws://127.0.0.1:18089/`。
- 鉴权实例（F11）连接 URL 带 `?token=<token>`（WsServer 支持的两种鉴权方式之一）。
- 收发：`ws.send(text)` / `ws.recv()`；`recv()` 阻塞至 timeout，超时抛 `WebSocketTimeoutException`（用于 E3 非法 action 用例：超时即通过）。
- 收到服务端关闭帧会抛异常，可据此判「连接是否被服务端主动关闭」（F11 无 token 被拒时需用）。

### 3.4 MCP 客户端（走 Streamable HTTP，用 requests）

本项目的 MCP 是 **Streamable HTTP**（2025-03-26 规范），本质是 HTTP POST JSON-RPC。**不引入官方 `mcp` SDK**——该 SDK 是 async 架构，在同步测试脚本里反而要套 asyncio 事件循环与会话管理，徒增复杂度。MCP 协议层这里只用到 `initialize` / `tools/list` / `tools/call` 三个方法，直接用 `requests` 发 JSON-RPC 最简洁透明：

```python
def mcp_call(mcp_port, method, params=None):
    r = requests.post(f"http://127.0.0.1:{mcp_port}/mcp",
                      json={"jsonrpc":"2.0","id":next_id(),"method":method,"params":params or {}})
    j = r.json()
    if "error" in j: raise McpError(j["error"])
    return j.get("result")
```

- 先发 `initialize`（带 clientInfo + protocolVersion `"2025-03-26"`），再 `tools/list`、`tools/call`。
- 带固定 session id（如 `mcp-test-1`）；cpp-mcp 已 patch 为 session 自动重建，但仍带上保持规范。
- 无密码实例的 MCP 端口不需要 token。

### 3.5 真实渲染的稳定性处理

搜狐首页 / Bing 搜索是真实公网页面，渲染耗时波动大（2–10s），偶发反爬：

- **长轮询窗口给足**：B 系列用 `long_poll_ms:45000`，HTTP 读超时设 50s。
- **超时给一次补取机会**（用户确认）：B1–B8 这些「期望成功」的用例，若首次返回 `state:running`，自动用 `GET /result/:id?output=...&timeout=25000` 补取一次；仍 running 才记失败。避免网络抖动假阴性。**B9（期望 running）不补取**——它是故意用 `long_poll_ms:1` 触发 running。
- **断言宽松**：只校验 state + 关键字段存在 + 非空，不校验具体内容（搜狐/Bing 内容会变）。
- **截图/PDF 校验二进制魔数**：下载后检首字节（`%PDF` / `\x89PNG` / JPEG `FF D8 FF`），不解析内容。

### 3.6 登录限流（F9）副作用隔离

F9 要打 10 次错误密码触发锁定，会污染实例 B 的 `/api/login`（锁 IP 10 分钟）。隔离方案：

- F9 排在 F 系列**最后**执行，F9 之后不再调用实例 B 的 `/api/login`。
- F4（正确登录换 token）排在 F9 **之前**，确保后续 F5–F8 的 token 已拿到。
- 实例 B 是脚本自启的临时实例，测完即销毁——这正是「脚本自管实例」的好处，可放心做破坏性验证。

### 3.7 cookie/proxy 测试的状态隔离

查 `main.cpp:377` 与 `CookieStore.cpp` 发现：CookieStore 的数据目录是 `applicationDirPath()/data`（`cookies.dat` + `seimi.key`），**没有命令行参数可改**。两个实例若共用同一二进制会共享 `build/data/`。

约束与处理：
- cookie（H 组）与 proxy（I 组）测试**只在无密码实例 A 上串行跑**（本来即按实例分组设计），实例 B 不碰 cookie/proxy。
- cookie 测试用临时域名 `regression-test.example`（不会与真实站点冲突）。
- 每组测试结束主动 `DELETE /cookies` 清空、`DELETE /proxy` 复位 direct；即便异常中断，残留的也是 `build/`（gitignore）内的测试假数据，无害。

## 4. 测试用例全集

共 9 组约 60 条用例。每条用例 = 一个断言点。「✓ 验证点」是该用例回归保护的核心行为。

### A. 健康与元信息接口（无密码实例 A）

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| A1 | 健康检查 | `GET /health` | 200 + `status:"ok"` |
| A2 | 鉴权状态-无密码 | `GET /auth-status` | `password_enabled:false` |
| A3 | 根路径管理页 | `GET /` | 200 + HTML（含 admin-ui 标志，如 `seimi-render` 或脚本引用） |
| A4 | 队列统计 | `GET /stats` | 含 `total/pending/running/done` 四字段 |
| A5 | 运行时全景状态 | `GET /status?domains=5` | 含 `uptime_ms/totals/latency_ms/queue/proxy/outputs/domains` |
| A6 | 安全响应头 | 任意请求检响应头 | 含 `X-Content-Type-Options:nosniff`、`X-Frame-Options:DENY`、CSP |

### B. 渲染核心链路（无密码实例 A，真实公网）

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| B1 | 搜狐首页 markdown | `POST /render` {url:sohu, output:markdown, long_poll_ms:45000} | `state:succeeded` + markdown 非空 + `md_algorithm_used` 存在 |
| B2 | 搜狐首页 html | output:html | succeeded + html 非空且含 `<html` |
| B3 | 搜狐首页 pdf | output:pdf | succeeded + `has_pdf:true`；`GET /pdf/:id` 返回 200 + `%PDF` 魔数 |
| B4 | 搜狐首页 screenshot png | output:screenshot, format:png | succeeded + `has_image:true` + `image_format:png`；`GET /image/:id` 返回 PNG 魔数 `\x89PNG` |
| B5 | 搜狐首页 screenshot auto | output:screenshot（默认 auto） | succeeded + has_image + format∈{png,jpeg}；image 魔数匹配 |
| B6 | Bing 搜索 readability | url:bing 搜索, output:markdown, md_algorithm:readability | succeeded + md 非空 + `md_algorithm_used∈{readability,conservative}`（允许回退） |
| B7 | 组合输出 | output:html,markdown,pdf,screenshot | 四项元信息（html/markdown/has_pdf/has_image）都齐全 |
| B8 | 非长轮询提交 | `POST /render` 不带 long_poll_ms | **响应立即返回**（不阻塞等待渲染）；state ∈ {pending, running, succeeded}（取决于提交瞬间调度速度），返回 `task_id` 可用于后续 `/status/:id` 查询 |
| B9 | 长轮询超时仍 running | long_poll_ms:1 | 返回 `state:running`（非 succeeded） |

Bing 搜索 URL 用 `https://www.bing.com/search?q=seimi+render`，搜狐首页用 `https://www.sohu.com/`。

### C. 任务查询接口（无密码实例 A）

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| C1 | 状态查询 | `GET /status/:id`（B1 的任务 id） | state=succeeded + 含 elapsed_ms + 不含 html 字段 |
| C2 | 长轮询取结果 | `GET /result/:id?output=markdown&timeout=25000`（已成功任务） | succeeded + markdown 非空 |
| C3 | 不存在任务-状态 | `GET /status/不存在id` | 404 + `error:"task not found"` |
| C4 | 不存在任务-结果 | `GET /result/不存在id` | 404 |
| C5 | pdf 未请求 | `GET /pdf/:id`（B1 的 markdown-only 任务） | 404 + 提示 `request output=pdf` |

### D. 参数校验与 SSRF 防护（无密码实例 A）

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| D1 | 缺 url | `POST /render` `{}` | 400 + `missing 'url'` |
| D2 | 非 http(s) | url:`ftp://x` | 400 + `url must be http/https` |
| D3 | 非法 JSON body | `POST /render` body=`not json` | 400 + `invalid json body` |
| D4 | SSRF-回环 | url:`http://127.0.0.1:8088` | 400 + `url blocked by SSRF guard` |
| D5 | SSRF-内网 | url:`http://10.0.0.1` | 400 + blocked by SSRF |
| D6 | SSRF-元数据 | url:`http://169.254.169.254` | 400 + blocked by SSRF |

### E. WebSocket 接口（无密码实例 A）

| # | 用例 | 流程 | ✓ 验证点 |
|---|------|------|---------|
| E1 | render 推送 | 连 WS → 发 `{action:render,url:sohu}` → 收 created → 收 finished | 两事件都收到 + finished 带 state:succeeded |
| E2 | subscribe 推送 | HTTP `/render` 提交（非长轮询）→ WS 发 `{action:subscribe,task_id}` → 收 subscribed → 收 finished | subscribed + finished 两事件 |
| E3 | 非法 action | 发 `{action:unknown}`，recv 设 3s timeout | 服务端不崩溃、连接未被服务端主动关闭：3s 内未收到 FIN/错误帧即通过（收到 error 帧也通过，只要连接存活） |

### F. 鉴权场景（密码实例 B）

> F 系列全部针对实例 B（`--password testpass_2024`）。token = sha256("seimi-render:" + password) 的 hex。

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| F1 | 鉴权状态-有密码 | `GET /auth-status` | `password_enabled:true` |
| F2 | 未带 token 被拒 | `GET /stats`（无 token） | 401 + 提示 unauthorized |
| F3 | 错误密码登录 | `POST /api/login` {password:wrong} | 401 + `invalid password` |
| F4 | 正确密码换 token | `POST /api/login` {password:testpass_2024} | 200 + `token` 字段非空 |
| F5 | Bearer token 鉴权 | `Authorization: Bearer <token>` 调 `/stats` | 200 |
| F6 | query token 鉴权 | `GET /stats?token=<token>` | 200 |
| F7 | 错误 token 被拒 | Bearer wrongtoken 调 `/stats` | 401 |
| F8 | token 确定性算法 | 本地算 sha256("seimi-render:"+pwd) 与 F4 返回 token 比对 | 一致（算法未变） |
| F9 | 登录限流 | 连续 10 次错误密码 | 第 1–10 次返回 401；**第 11 次返回 429 + Retry-After 头** |
| F10 | 免鉴权路径 | 无 token 调 `/health`、`/auth-status`、`/api/login`、`/` | 全部非 401 |
| F11 | WS 鉴权 | 无 token 连 WS 发 render → 收 error 或连接被关；带 `?token=<token>` → render 成功 | 鉴权生效（无 token 被拒 / 有 token 成功） |
| F12 | Cookie token | `Cookie: seimi_token=<token>` 调 `/stats` | 200 |

F9 必须排在 F4–F8 之后（确保 token 已拿到）；F9 之后再不调用实例 B 的 `/api/login`。

### G. MCP 接口（无密码实例 A 的 MCP 端口 18090）

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| G1 | tools/list | MCP initialize + `tools/list` | 含 `render_url` + `get_render_result` 两个工具 |
| G2 | render_url markdown | `tools/call` render_url(sohu, output:markdown) | 返回 text content + 非空 markdown |
| G3 | render_url screenshot | render_url(sohu, output:screenshot) | 含 image content（base64，mimeType image/png 或 image/jpeg） |
| G4 | get_render_result | 用 G2 的 task_id 调 get_render_result(output:markdown) | 返回 markdown 内容 |
| G5 | 缺参数 | render_url 无 url | MCP `invalid_params` 错误 |

### H. Cookie 接口（无密码实例 A）

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| H1 | 批量注入 | `POST /cookies` {cookies:[{name:k1,domain:regression-test.example,value:v1,path:/}]} | `stored>=1` + `applied:true` |
| H2 | 概览 | `GET /cookies` | `total>=1` + domains 含 `regression-test.example` |
| H3 | 清空 | `DELETE /cookies` | `cleared:true`；再 GET `/cookies` total=0 |

### I. Proxy 接口（无密码实例 A）

> 不真正走代理流量（无可用代理服务器），只验证配置读写往返。测试结束 `DELETE /proxy` 复位 direct。

| # | 用例 | 请求 | ✓ 验证点 |
|---|------|------|---------|
| I1 | 默认直连 | `GET /proxy` | `type:direct` |
| I2 | 设置 socks5 | `POST /proxy` {type:socks5,host:127.0.0.1,port:1080} | `ok:true`；GET 回显 `type:socks5` + host/port |
| I3 | 非法端口 | `POST /proxy` {type:http,host:x,port:99999} | 400 + `requires 'host' and 'port'` |
| I4 | 恢复直连 | `DELETE /proxy` | `ok:true`；GET 回 `type:direct` |

## 5. 命令行接口

```bash
# 默认：构建 + 起两实例 + 全测 + 收尾 + 报告
python scripts/regression_test.py

# 常用旗标
python scripts/regression_test.py --skip-build        # 复用上次构建产物（开发期迭代快）
python scripts/regression_test.py --no-auth           # 只测无密码实例（跳过 F 系列，更快）
python scripts/regression_test.py --group B           # 只跑某一组（A/B/C/D/E/F/G/H/I）
python scripts/regression_test.py --case B1,B6        # 只跑指定用例
python scripts/regression_test.py --keep-servers      # 测完不杀进程（调试用，配合手动验证）
python scripts/regression_test.py --verbose           # 打印每个请求/响应详情（调试用）
python scripts/regression_test.py --report-dir build/test-reports   # 自定义报告目录（默认即此）
```

默认行为面向「改完代码一键验证」：构建 + 全测，无需任何参数。

## 6. 输出与报告

### 6.1 终端输出（实时）

```
== seimi-render 全接口回测 ==
[build] 调用 scripts/build-windows.bat ... ✓ (12.3s)
[boot]  实例A(无密码) http://127.0.0.1:18088 ... 就绪 (2.1s)
[boot]  实例B(有密码) http://127.0.0.1:18188 ... 就绪 (2.3s)

A. 健康与元信息
  ✓ A1 健康检查                        (12ms)
  ✓ A2 鉴权状态-无密码                  (8ms)
  ...
B. 渲染核心链路
  ✓ B1 搜狐首页 markdown               (4523ms)
  ✗ B3 搜狐首页 pdf                     (60123ms)  state=running after retry
  ...

== 结果汇总 ==
  总计 60 | 通过 58 | 失败 2 | 耗时 312s
  失败：B3 (搜狐首页 pdf)、I2 (设置 socks5)
报告已写入 build/test-reports/regression-20260628-153012.json
exit 1
```

### 6.2 JSON 报告

```json
{
  "started_at": "2026-06-28T15:30:12",
  "duration_ms": 312456,
  "build": {"ok": true, "binary": "build/seimi-render.exe", "duration_ms": 12300},
  "instances": {
    "no_auth": {"http_port": 18088, "ws_port": 18089, "mcp_port": 18090, "pid": 12345, "ready": true},
    "auth":    {"http_port": 18188, "ws_port": 18189, "mcp_port": 18190, "pid": 12346, "ready": true}
  },
  "summary": {"total": 60, "passed": 58, "failed": 2, "skipped": 0},
  "groups": [
    {"name": "A. 健康与元信息", "passed": 6, "failed": 0, "cases": [
      {"id": "A1", "name": "健康检查", "status": "pass", "duration_ms": 12,
       "assertion": "GET /health → 200 + status:ok"}
    ]},
    {"name": "B. 渲染核心链路", "passed": 8, "failed": 1, "cases": [
      {"id": "B3", "name": "搜狐首页 pdf", "status": "fail", "duration_ms": 60123,
       "assertion": "POST /render output:pdf → succeeded + has_pdf + /pdf 魔数",
       "error": "state=running after retry, has_pdf missing",
       "detail": {
         "request": {"method": "POST", "path": "/render", "body": {"url": "...", "output": "pdf"}},
         "response": {"status": 200, "body": "{\"state\":\"running\",...}"},
         "elapsed_ms": 60123
       }}
    ]}
  ]
}
```

### 6.3 退出码

| 退出码 | 含义 |
|--------|------|
| 0 | 全部用例通过 |
| 1 | 有用例失败（功能回归） |
| 2 | 构建失败或实例启动失败（环境问题，非功能回归） |

区分 1 和 2 的意义：CI 里 code 2 应触发「环境坏了」告警，而非「代码改坏了」告警。

## 7. 文件落点

```
scripts/
├── regression_test.py        ← 新增，主脚本（单文件，依赖 requests + websocket-client）
├── smoke_test.sh             （现有，保留不动）
└── soak_test.py              （现有，保留不动）

build/test-reports/           ← JSON 报告输出（.gitignore，构建产物区）
└── regression-YYYYMMDD-HHMMSS.json
```

单文件不拆模块——60 条用例约 800 行，可读性可控，且方便一键跑。报告输出到 `build/`（已是 gitignore 的构建产物区），不污染源码树。

## 8. 不在本次范围内（YAGNI）

以下刻意排除，避免过度设计：

- **不引入 pytest 等测试框架**：标准库 + 自写断言 + `requests`/`websocket-client` 即可，避免测试框架带来的固定结构与样板。
- **不解析渲染内容像素/布局**：只校验魔数与字段存在性，内容会变，强校验反而制造噪音。
- **不测 stealth 指纹效果**（creepjs 等）：依赖外部反爬检测站点的实时判定，不稳定，且非接口行为。
- **不测并发压测**：那是 `soak_test.py` 的职责，本脚本聚焦接口正确性。
- **不在密码实例上测 MCP 鉴权**：MCP token 与 HTTP 同源，已被 F 系列覆盖，避免重复。
- **不测 trusted-proxies 的 XFF 解析**：需要构造可信代理链路，环境复杂，且属配置边缘场景。
