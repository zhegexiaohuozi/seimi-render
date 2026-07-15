# 设计：管理界面「运行环境」信息卡片

- **日期**：2026-07-05
- **状态**：已与用户确认设计，待评审 spec
- **范围**：仅后端环境采集 + 前端展示。原需求的 `-d` daemon 模式与日志重定向已取消。

## 1. 目标

在管理界面「运行时统计」页签（`tab-stats`）最末新增一个「运行环境」卡片，展示当前进程所在主机的 OS / 内核 / CPU / 内存 / GPU / Qt 版本 / 构建信息，以及本进程的实时 CPU 占用率与内存占用。提供一键拷贝为 Markdown 代码块，方便运维人员提交环境信息做问题排查。

## 2. 非目标

- **不做** `-d` daemon 后台化（已取消）。
- **不做** 日志文件落盘 / 滚动更新（已取消）。
- **不做** 磁盘 / 网络 / 进程列表等扩展采集（YAGNI，当前诊断需求只到 CPU/内存）。
- **不做** GPU 具体型号采集（macOS/Windows 一律标 `has_gpu=true`，Linux 探测有无；型号跨平台实现成本高、诊断价值低）。

## 3. 架构

新增独立模块 `Environment`，与 `Metrics` / `ProxyConfig` 同模式：单例式（实例由 `main.cpp` 持有）、自带锁、跨线程安全、启动时采集不变量 + GUI 定时器采样实时值。

```
main.cpp
  ├─ Environment env          （构造即采集静态字段）
  │    └─ env.start(&app)      （启动 GUI 线程 3s 采样定时器）
  ├─ HttpServer http(..., &env)（/status 调 env.snapshot()）
  └─ RenderPool ...
```

**为何独立模块**：
- `HttpServer::jsonRuntimeStatus` 已 100+ 行，再塞环境采集会臃肿。
- 环境采集涉及大量平台原生 API（`/proc`、`GetSystemInfo`、`sysctl`），独立模块便于把 `#ifdef` 噪音锁在 `.cpp` 内，header 保持干净。

**线程模型**（符合 AGENTS.md 五条铁律）：
- 静态字段：构造时在主线程采集一次（启动早期，QApplication 之后）。
- 实时采样：`QTimer`（3s 间隔）由 GUI 事件循环驱动，单次采样 < 1ms（纯文件读 / syscall），不违反「GUI 线程不阻塞」。
- 跨线程读取：`snapshot()` 内部加锁返回拷贝，HTTP 线程（httplib 工作线程）安全调用。

## 4. 数据字段

`Environment::snapshot()` 返回 `Snapshot` 结构，最终序列化进 `/status` 的 `environment` 对象。

### 4.1 静态字段（启动时采集一次）

| 字段 | 类型 | 采集方式 | 失败默认 |
|---|---|---|---|
| `os_name` | string | `QSysInfo::productType()` | `""` |
| `os_version` | string | `QSysInfo::productVersion()` | `""` |
| `os_pretty` | string | `QSysInfo::prettyProductName()` | `""` |
| `kernel` | string | `QSysInfo::kernelVersion()` | `""` |
| `arch` | string | `QSysInfo::currentCpuArchitecture()` | `""` |
| `build_arch` | string | `QSysInfo::buildCpuArchitecture()` | `""` |
| `hostname` | string | `QSysInfo::machineHostName()` | `""` |
| `cpu_model` | string | 平台原生（见 §5） | `""` |
| `cpu_cores_logical` | int | `QThread::idealThreadCount()` | `0` |
| `cpu_cores_physical` | int | 平台原生（见 §5） | `0` |
| `memory_total_mb` | int (MB) | 平台原生（见 §5） | `0` |
| `has_gpu` | bool | 平台原生（见 §5） | `false` |
| `qt_version` | string | `qVersion()` | `""` |
| `build_time` | string | `SEIMI_BUILD_TIME` 宏（`#ifndef` 兜底 `unknown`） | `unknown` |
| `git_commit` | string | `SEIMI_GIT_COMMIT`（若 `SEIMI_GIT_DIRTY=="dirty"` 则追加 ` (dirty)`；`clean` 时不追加） | `unknown` |
| `started_at_ms` | int64 | `QDateTime::currentMSecsSinceEpoch()` 于构造时 | — |

### 4.2 实时字段（GUI 定时器每 3s 采样）

| 字段 | 类型 | 采集方式 | 失败默认 |
|---|---|---|---|
| `cpu_percent` | double | 本进程 CPU 时间差分 / 墙钟时间差分 × 100 | `0` |
| `memory_rss_mb` | int (MB) | 平台原生（见 §5） | `0` |
| `memory_percent` | double | `rss_mb / memory_total_mb × 100` | `0` |
| `sampled_at_ms` | int64 | 采样时刻 `QDateTime::currentMSecsSinceEpoch()`；首次采样前为 `0` | `0` |

**未采样语义**：进程刚启动到第一次采样（最多 3s）之间，`sampled_at_ms == 0`，前端据此区分「未采样」与「真 0%」。第二次采样（约 6s 后）`cpu_percent` 才有差分值。

### 4.3 Markdown 预拼字段

`Snapshot` 同时携带 `markdown` 字符串，后端预拼好（不依赖 i18n，固定中文模板）。理由：
- 字段顺序 / 格式跨端一致。
- i18n 翻译不污染拷贝输出（前端显示用翻译，拷贝用固定模板）。
- 改格式只改一处（`Environment::toMarkdown()`）。

模板：

```markdown
## seimi-render 运行环境

- **OS**: <os_pretty> (<arch>)
- **Kernel**: <kernel>
- **Hostname**: <hostname>
- **CPU**: <cpu_model> · <phys>物理核/<logical>逻辑核
- **内存**: <memory_total_mb> MB
- **GPU**: 无（headless 服务器） / 有
- **Qt 版本**: <qt_version>
- **构建信息**: <build_time> · <git_commit>
- **CPU 占用**: <cpu_percent>%           ← 未采样时显示 "--"
- **内存占用**: <memory_rss_mb> MB (<memory_percent>%)  ← 未采样时显示 "--"
- **采样时间**: <sampled_at_ms 格式化为本地时间>

> 由 seimi-render 管理界面「运行环境」卡片一键导出
```

## 5. 平台原生采集实现

**核心原则**：每个采集函数用 `#ifdef Q_OS_*` 隔离，**所有调用都包失败返回默认值**，单点失败不影响其它字段。`#ifdef` 全部锁在 `Environment.cpp` 内。

### 5.1 CPU 型号

| 平台 | 实现 |
|---|---|
| Linux | 读 `/proc/cpuinfo`，找首条 `model name` 行（x86/ARM 通用） |
| macOS | `sysctlbyname("machdep.cpu.brand_string", ...)`（Apple Silicon 返回 `Apple M2 Pro` 等） |
| Windows | `RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", ...)` |

### 5.2 物理核数

| 平台 | 实现 |
|---|---|
| Linux | 解析 `/proc/cpuinfo`：把 `(physical id, core id)` 组合去重计数（一个组合 = 一个物理核）。若文件无 `physical id` / `core id` 字段（罕见内核配置），fallback 回 `QThread::idealThreadCount()` 并标 `physical==logical` |
| macOS | `sysctlbyname("hw.physicalcpu", ...)` |
| Windows | `GetLogicalProcessorInformationEx(RelationProcessorPackage, ...)` 数包数；fallback `QThread::idealThreadCount()`（= 逻辑核） |

### 5.3 内存总量

| 平台 | 实现 |
|---|---|
| Linux | `/proc/meminfo` 的 `MemTotal:` 字段（kB 单位） |
| macOS | `sysctlbyname("hw.memsize", ...)`（bytes） |
| Windows | `GlobalMemoryStatusEx(&mem)` → `mem.ullTotalPhys`（bytes） |

### 5.4 GPU 探测（仅 `has_gpu`）

| 平台 | 实现 |
|---|---|
| Linux | `/proc/driver/nvidia/version` 存在 → `true`；否则尝试 `popen("lspci")` grep `VGA` / `3D`；都失败 → `false`（headless 服务器） |
| macOS | 一律 `true`（macOS 都有集显 / 独显，IORegistry 枚举太重） |
| Windows | 一律 `true`（装机必有显卡驱动） |

### 5.5 实时 CPU% 与 RSS

**采样逻辑**（`Environment::sample()`，GUI 线程调用）：

```
proc_time = currentProcCpuTime()   // 平台原生，返回累计 CPU 时间（秒）
wall_ms   = QDateTime::currentMSecsSinceEpoch()
rss_mb    = currentProcRssMb()     // 平台原生

if (m_lastWallMs > 0) {  // 有基线
    cpu_percent = (proc_time - m_lastProcTime) / ((wall_ms - m_lastWallMs) / 1000.0) * 100.0
}
m_lastProcTime = proc_time
m_lastWallMs   = wall_ms
m_lastRssMb    = rss_mb
// 加锁写 RuntimeSample（含 cpu_percent、rss、memory_percent、sampled_at_ms）
```

| 平台 | CPU 时间 | RSS |
|---|---|---|
| Linux | `/proc/self/stat` 第 14+15 字段（utime+stime，clock tick）÷ `sysconf(_SC_CLK_TCK)` → 秒 | `/proc/self/status` 的 `VmRSS:` 行（kB） |
| macOS | `proc_pid_rusage(pid, RUSAGE_INFO_CURRENT, ...)` → `ri_user_time + ri_system_time`（nanoseconds）→ 秒 | 同上 `ri_resident_size`（bytes） |
| Windows | `GetProcessTimes()`（FILETIME `lpUserTime + lpKernelTime`，100ns 单位）→ 秒 | `GetProcessMemoryInfo()` 的 `WorkingSetSize`（bytes） |

**采样定时器**：`Environment` 内部 `QTimer`，3s 间隔，`moveToThread(qApp->thread())`，由 GUI 事件循环驱动。**不复用** `RenderPool::m_proxyApplyTimer`（职责分离，避免采样异常波及代理应用）。

## 6. 后端接入

### 6.1 `HttpServer` 改动

- 构造函数新增可选参数 `Environment* env = nullptr`（或新增 `setEnvironment(Environment*)` setter，避免破坏现有构造签名）—— **推荐 setter**，与 `setTrustedProxies` 同模式。
- `jsonRuntimeStatus()` 末尾追加 `environment` 段：

```cpp
if (m_env) {
    Environment::Snapshot e = m_env->snapshot();
    s += ",\"environment\":{";
    // 静态字段（字符串全过 escJson）
    s += "\"os_name\":\"" + escJson(e.osName) + "\"";
    s += ",\"os_version\":\"" + escJson(e.osVersion) + "\"";
    s += ",\"os_pretty\":\"" + escJson(e.osPretty) + "\"";
    s += ",\"kernel\":\"" + escJson(e.kernel) + "\"";
    s += ",\"arch\":\"" + escJson(e.arch) + "\"";
    s += ",\"build_arch\":\"" + escJson(e.buildArch) + "\"";
    s += ",\"hostname\":\"" + escJson(e.hostname) + "\"";
    s += ",\"cpu_model\":\"" + escJson(e.cpuModel) + "\"";
    s += ",\"cpu_cores_logical\":" + std::to_string(e.cpuLogicalCores);
    s += ",\"cpu_cores_physical\":" + std::to_string(e.cpuPhysicalCores);
    s += ",\"memory_total_mb\":" + std::to_string(e.memoryTotalMb);
    s += ",\"has_gpu\":" + std::string(e.hasGpu ? "true" : "false");
    s += ",\"qt_version\":\"" + escJson(e.qtVersion) + "\"";
    s += ",\"build_time\":\"" + escJson(e.buildTime) + "\"";
    s += ",\"git_commit\":\"" + escJson(e.gitCommit) + "\"";
    // 实时字段
    s += ",\"cpu_percent\":" + fmtDouble(e.cpuPercent);
    s += ",\"memory_rss_mb\":" + std::to_string(e.memoryRssMb);
    s += ",\"memory_percent\":" + fmtDouble(e.memoryPercent);
    s += ",\"sampled_at_ms\":" + fmtMs(e.sampledAtMs);
    s += ",\"markdown\":\"" + escJson(e.markdown) + "\"";
    s += "}";
}
```

- **鉴权**：随 `/status` 走，已有 `SEIMI_AUTH` 守护，无需额外处理。
- **响应体积**：`environment` 段约 600-800 字节，`/status` 原本 1-3KB，可忽略。

### 6.2 `main.cpp` 改动

```cpp
// 在 HttpServer 创建之前（构造 env 需要 QApplication 已存在）
Environment env;       // 构造时采集静态字段
env.start(&app);       // 启动 3s GUI 采样定时器

HttpServer http(&queue, &cookies, cfg.settleDefaultMs,
                adminUi, cfg.adminPassword, &app);
http.setEnvironment(&env);
```

`Environment` 继承 `QObject`，`start(QObject* parent)` 把 `QTimer` 父对象挂到 `&app`，随 app 退出自动清理。

### 6.3 `CMakeLists.txt` 改动

`src/Environment.cpp` 加入 `seimi-render` 目标源码列表（与 `Metrics.cpp` / `ProxyConfig.cpp` 同列）。

## 7. 前端实现

### 7.1 HTML（`admin-ui/index.html`）

在 `tab-stats` 内、域名分布表格 `section` 之后、`</section>`（页签关闭）之前，新增：

```html
<div class="section">
  <h3>
    <svg viewBox="0 0 24 24" width="18" height="18"><!-- 电脑/服务器图标 --></svg>
    <span data-i18n="stats.env.title">运行环境</span>
    <span id="stats-env-sampled" class="muted small" style="font-weight:normal; margin-left:4px;"></span>
    <button id="env-copy-btn" class="copy-btn" data-i18n="stats.env.copy" title="">📋 拷贝</button>
    <span id="env-copy-status" class="copy-status"></span>
  </h3>
  <div id="stats-env" class="env-grid">
    <p class="muted small" data-i18n="common.loading">加载中…</p>
  </div>
</div>
```

### 7.2 JS（`admin-ui/app.js`）

在现有 `renderStats(d)` 函数内追加 `renderEnv(d.environment)` 调用，并新增：

```js
let lastEnvMarkdown = '';

function renderEnv(env) {
  const el = document.getElementById('stats-env');
  if (!env) { el.innerHTML = ''; return; }
  lastEnvMarkdown = env.markdown || '';

  const rows = [
    [t('stats.env.os'),       `${esc(env.os_pretty)} (${esc(env.arch)})`],
    [t('stats.env.kernel'),   esc(env.kernel)],
    [t('stats.env.hostname'), esc(env.hostname)],
    [t('stats.env.cpu'),      `${esc(env.cpu_model)} · ${env.cpu_cores_physical}${t('stats.env.coresPhys')}/${env.cpu_cores_logical}${t('stats.env.coresLogical')}`],
    [t('stats.env.memory'),   `${env.memory_total_mb} MB`],
    [t('stats.env.gpu'),      env.has_gpu ? t('stats.env.gpuYes') : t('stats.env.gpuNo')],
    [t('stats.env.qt'),       esc(env.qt_version)],
    [t('stats.env.build'),    `${esc(env.build_time)} · ${esc(env.git_commit)}`],
    [t('stats.env.cpuUsage'), env.sampled_at_ms > 0 ? `${env.cpu_percent.toFixed(1)}%` : '--'],
    [t('stats.env.rss'),      env.sampled_at_ms > 0 ? `${env.memory_rss_mb} MB (${env.memory_percent.toFixed(1)}%)` : '--'],
  ];
  el.innerHTML = '<table class="env-table"><tbody>' +
    rows.map(([k,v]) => `<tr><td class="k">${k}</td><td class="v">${v}</td></tr>`).join('') +
    '</tbody></table>';

  document.getElementById('stats-env-sampled').textContent =
    env.sampled_at_ms > 0
      ? t('stats.env.sampledAt', { ts: new Date(env.sampled_at_ms).toLocaleTimeString() })
      : '';
}

// 拷贝按钮（含 execCommand 兜底）
async function copyEnv() {
  if (!lastEnvMarkdown) return;
  try {
    await navigator.clipboard.writeText(lastEnvMarkdown);
    showCopyStatus(t('stats.env.copied'), 'ok');
  } catch (e) {
    // clipboard API 在非安全上下文（http://非 localhost）下不可用，必须兜底
    const ta = document.createElement('textarea');
    ta.value = lastEnvMarkdown;
    ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.select();
    let ok = false;
    try { ok = document.execCommand('copy'); } catch (e2) {}
    document.body.removeChild(ta);
    showCopyStatus(ok ? t('stats.env.copied') : t('stats.env.copyFail'), ok ? 'ok' : 'err');
  }
}

function showCopyStatus(msg, cls) {
  const s = document.getElementById('env-copy-status');
  s.textContent = msg; s.className = 'copy-status ' + cls;
  setTimeout(() => { s.textContent = ''; s.className = 'copy-status'; }, 2000);
}

document.getElementById('env-copy-btn').addEventListener('click', copyEnv);
```

**关键**：`navigator.clipboard` 在 `http://172.16.88.93:8088/` 这类非安全上下文下被浏览器禁用，**必须** `execCommand('copy')` 兜底——这是 seimi-render 内网部署的典型场景。

### 7.3 CSS（`admin-ui/app.css`）

```css
.env-grid { font-size: 13px; }
.env-table { width: 100%; border-collapse: collapse; }
.env-table td { padding: 4px 8px; border-bottom: 1px solid var(--border); vertical-align: top; }
.env-table td.k { color: var(--muted); white-space: nowrap; width: 120px; }
.env-table td.v { word-break: break-all; }
.copy-btn { margin-left: 8px; padding: 2px 10px; font-size: 12px; cursor: pointer; }
.copy-status { margin-left: 6px; font-size: 12px; }
.copy-status.ok { color: var(--ok, #2c8); }
.copy-status.err { color: var(--err, #c42); }
```

### 7.4 i18n（`admin-ui/i18n.js`）

新增 `stats.env.*` 键组，中英文各一套，与现有 `stats.cards.*` / `stats.latency.*` 同结构。

键清单：`title` / `copy` / `copied` / `copyFail` / `os` / `kernel` / `hostname` / `cpu` / `coresPhys` / `coresLogical` / `memory` / `gpuYes` / `gpuNo` / `qt` / `build` / `cpuUsage` / `rss` / `sampledAt`。

## 8. 打包脚本影响

`Environment.cpp` 编进二进制，无新增运行时磁盘资源——三个打包脚本（`package-linux.sh` / `package.sh` / `package-windows.bat`）**无需改动**。

## 9. 测试

- **手动验证**：三平台（Linux 服务器、macOS、Windows）各启动一次，访问 `/status`，确认 `environment` 段字段齐全、无空值异常；管理界面卡片渲染正常；拷贝粘贴 Markdown 格式正确。
- **边界**：headless Linux 服务器（无 GPU）`has_gpu=false`；进程刚启动 `< 3s` 时卡片 CPU/RSS 显示 `--`；非安全上下文 `http://` 下拷贝走 `execCommand` 兜底。
- **回归**：`/status` 原有字段不受影响；无 env 时（`m_env==nullptr`）`environment` 段省略，前端卡片区清空。

## 10. 改动清单

| 文件 | 改动类型 | 说明 |
|---|---|---|
| `src/Environment.h` | 新增 | `Environment` 类声明、`Snapshot` 结构 |
| `src/Environment.cpp` | 新增 | 实现 + 平台 `#ifdef` 采集 + `toMarkdown()` |
| `src/HttpServer.h` | 修改 | 新增 `m_env` 成员 + `setEnvironment()` |
| `src/HttpServer.cpp` | 修改 | `jsonRuntimeStatus()` 追加 `environment` 段 |
| `src/main.cpp` | 修改 | 创建 `Environment`、`env.start(&app)`、`http.setEnvironment(&env)` |
| `CMakeLists.txt` | 修改 | 加 `src/Environment.cpp` |
| `admin-ui/index.html` | 修改 | 新增「运行环境」section |
| `admin-ui/app.js` | 修改 | `renderEnv()` / `copyEnv()` / 事件绑定 |
| `admin-ui/app.css` | 修改 | `.env-table` / `.copy-btn` / `.copy-status` |
| `admin-ui/i18n.js` | 修改 | `stats.env.*` 键组 |
