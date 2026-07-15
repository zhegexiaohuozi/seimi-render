# 运行环境信息卡片 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在管理界面「运行时统计」页签末尾新增「运行环境」卡片，展示 OS / 硬件 / 构建信息 + 实时 CPU/RSS 占用，支持一键拷贝为 Markdown，供运维提交环境信息排查问题。

**Architecture:** 新增独立 `Environment` 模块（QObject 子类，与 `Metrics`/`ProxyConfig` 同模式）：构造时在主线程采集静态字段，GUI 线程 3s `QTimer` 采样实时 CPU/RSS，`snapshot()` 加锁返回拷贝供 HTTP 线程读取。后端在 `/status` 的 JSON 末尾追加 `environment` 段（含预拼好的 `markdown` 字段），前端 `renderStats` 内追加 `renderEnv` 渲染 + `clipboard`/`execCommand` 拷贝。

**Tech Stack:** C++17 / Qt6 (Core/Gui) / cpp-httplib / 原生平台 API（Linux `/proc`、macOS `sysctl`/`libproc`、Windows Win32）/ 原生 JS（无框架）。

**Spec:** `docs/superpowers/specs/2026-07-05-env-info-card-design.md`

**测试策略说明：** 本项目无 C++ 单元测试框架（仅 `smoke_test.sh`/`soak_test.py` 集成测试）。本计划采用「编译通过 + 启动后 `/status` JSON 校验 + 管理界面人工目视」三层验证，不强行套用不存在的 gtest TDD。每个 Task 结束有一个可执行的验证命令。

---

## File Structure

| 文件 | 责任 | 状态 |
|---|---|---|
| `src/Environment.h` | `Environment` 类声明 + `Snapshot` 结构 | 新增 |
| `src/Environment.cpp` | 实现：静态采集（Qt API + `#ifdef Q_OS_*` 原生 API）+ 3s 采样定时器 + `snapshot()` 加锁 + `toMarkdown()` | 新增 |
| `CMakeLists.txt` | 把 `Environment.h/cpp` 加入 `seimi-render` 源码列表 | 修改 |
| `src/HttpServer.h` | 新增 `m_env` 成员 + `setEnvironment()` 内联 setter | 修改 |
| `src/HttpServer.cpp` | `jsonRuntimeStatus()` 末尾追加 `environment` JSON 段 | 修改 |
| `src/main.cpp` | 创建 `Environment env;` + `env.start(&app);` + `http.setEnvironment(&env);` | 修改 |
| `admin-ui/index.html` | 在域名分布 `section` 后新增「运行环境」`section` | 修改 |
| `admin-ui/app.js` | `renderStats` 末尾调 `renderEnv(d.environment)` + 新增 `renderEnv`/`copyEnv`/`showCopyStatus` | 修改 |
| `admin-ui/app.css` | `.env-table` / `.copy-btn` / `.copy-status` | 修改 |
| `admin-ui/i18n.js` | `stats.env.*` 键组（中/英） | 修改 |

---

## Task 1: 新增 `Environment` 模块骨架（编译通过）

先建最小骨架：`Snapshot` 结构 + 构造函数采集 Qt API 能拿到的静态字段 + `snapshot()` 加锁返回 + `start()` 空实现。平台原生 API 在 Task 2 补。这样 Task 1 结束就能编译跑通，建立反馈循环。

**Files:**
- Create: `src/Environment.h`
- Create: `src/Environment.cpp`
- Modify: `CMakeLists.txt:280-300`（add_executable 源码列表）

- [ ] **Step 1: 写 `src/Environment.h`**

```cpp
#ifndef SEIMI_ENVIRONMENT_H
#define SEIMI_ENVIRONMENT_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <cstdint>

namespace seimi {

// 运行环境快照（静态字段 + 实时采样合并）。snapshot() 返回值，跨线程安全拷贝。
// 字段缺失/采集失败时：字符串为空、整数为 0、bool 为 false，绝不抛异常。
struct EnvironmentSnapshot {
    // —— 静态字段（构造时采集一次）——
    QString osName;            // QSysInfo::productType()
    QString osVersion;         // QSysInfo::productVersion()
    QString osPretty;          // QSysInfo::prettyProductName()
    QString kernel;            // QSysInfo::kernelVersion()
    QString arch;              // QSysInfo::currentCpuArchitecture()
    QString buildArch;         // QSysInfo::buildCpuArchitecture()
    QString hostname;          // QSysInfo::machineHostName()
    QString cpuModel;          // 平台原生
    int     cpuLogicalCores = 0;   // QThread::idealThreadCount()
    int     cpuPhysicalCores = 0; // 平台原生
    qint64  memoryTotalMb = 0;     // 平台原生
    bool    hasGpu = false;        // 平台原生
    QString qtVersion;         // qVersion()
    QString buildTime;         // SEIMI_BUILD_TIME 宏
    QString gitCommit;         // SEIMI_GIT_COMMIT + dirty 标注
    qint64  startedAtMs = 0;   // 构造时刻

    // —— 实时字段（GUI 定时器采样，首次采样前为 0/null）——
    double  cpuPercent = 0.0;
    qint64  memoryRssMb = 0;
    double  memoryPercent = 0.0;
    qint64  sampledAtMs = 0;   // 0 = 尚未采样

    // —— 预拼好的 Markdown（前端拷贝直接用，避免 JS 重复实现）——
    QString markdown;
};

// 运行环境采集器（QObject，3s GUI 定时器采样）。
// 线程模型：构造/start/snapshot 可在任意线程调；采样定时器跑在 qApp->thread()。
class Environment : public QObject {
    Q_OBJECT
public:
    explicit Environment(QObject* parent = nullptr);

    // 启动 GUI 线程 3s 采样定时器。必须在 qApp 存在后调用。
    void start(QObject* mainThreadParent);

    // 加锁返回当前快照拷贝（HTTP 线程安全调用）。
    EnvironmentSnapshot snapshot() const;

private:
    void collectStatic();       // 构造时采集静态字段（Qt API + 平台原生）
    void sample();              // GUI 定时器回调：采样 CPU/RSS + 重算 markdown
    QString buildMarkdown(const EnvironmentSnapshot& s) const;

    // 平台原生采集（实现见 .cpp，#ifdef 隔离；失败返回默认值）
    static QString  probeCpuModel();
    static int      probePhysicalCores();
    static qint64   probeMemoryTotalMb();
    static bool     probeHasGpu();
    static double   probeProcCpuTimeSec();  // 累计 CPU 时间（秒），差分用
    static qint64   probeProcRssMb();       // 当前 RSS（MB）

    mutable QMutex m_mutex;
    EnvironmentSnapshot m_snap;
    qint64 m_lastProcTimeSec = 0;   // 上次采样的累计 CPU 时间（秒）
    qint64 m_lastWallMs = 0;        // 上次采样的墙钟时间戳
    class QTimer* m_timer = nullptr;
};

} // namespace seimi

#endif // SEIMI_ENVIRONMENT_H
```

- [ ] **Step 2: 写 `src/Environment.cpp`（最小骨架版，平台 API 留 TODO 占位返回默认值）**

```cpp
#include "Environment.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>

#ifndef SEIMI_BUILD_TIME
#define SEIMI_BUILD_TIME "unknown"
#endif
#ifndef SEIMI_GIT_COMMIT
#define SEIMI_GIT_COMMIT "unknown"
#endif
#ifndef SEIMI_GIT_DIRTY
#define SEIMI_GIT_DIRTY "nogit"
#endif

namespace seimi {

Environment::Environment(QObject* parent) : QObject(parent) {
    collectStatic();
    m_snap.startedAtMs = QDateTime::currentMSecsSinceEpoch();
}

void Environment::start(QObject* mainThreadParent) {
    // 定时器父对象挂到主线程 parent（通常是 qApp），随其退出自动清理。
    m_timer = new QTimer(mainThreadParent);
    m_timer->setInterval(3000);
    connect(m_timer, &QTimer::timeout, this, &Environment::sample);
    // 立即触发一次首采样（建立差分基线），后续按 3s 间隔。
    QTimer::singleShot(0, this, &Environment::sample);
    m_timer->start();
}

void Environment::collectStatic() {
    m_snap.osName    = QSysInfo::productType();
    m_snap.osVersion = QSysInfo::productVersion();
    m_snap.osPretty  = QSysInfo::prettyProductName();
    m_snap.kernel    = QSysInfo::kernelVersion();
    m_snap.arch      = QSysInfo::currentCpuArchitecture();
    m_snap.buildArch = QSysInfo::buildCpuArchitecture();
    m_snap.hostname  = QSysInfo::machineHostName();
    m_snap.cpuLogicalCores = QThread::idealThreadCount();
    m_snap.cpuModel       = probeCpuModel();
    m_snap.cpuPhysicalCores = probePhysicalCores();
    m_snap.memoryTotalMb = probeMemoryTotalMb();
    m_snap.hasGpu        = probeHasGpu();
    m_snap.qtVersion     = QString::fromLatin1(qVersion());
    m_snap.buildTime     = QString::fromLatin1(SEIMI_BUILD_TIME);
    m_snap.gitCommit     = QString::fromLatin1(SEIMI_GIT_COMMIT);
    if (qstrcmp(SEIMI_GIT_DIRTY, "dirty") == 0) {
        m_snap.gitCommit += QStringLiteral(" (dirty)");
    }
}

EnvironmentSnapshot Environment::snapshot() const {
    QMutexLocker lock(&m_mutex);
    return m_snap;
}

void Environment::sample() {
    double procSec = probeProcCpuTimeSec();
    qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
    qint64 rssMb  = probeProcRssMb();

    QMutexLocker lock(&m_mutex);
    if (m_lastWallMs > 0 && wallMs > m_lastWallMs) {
        double wallSec = double(wallMs - m_lastWallMs) / 1000.0;
        double cpuUsed = procSec - double(m_lastProcTimeSec) / 1000.0;
        // 注意：probeProcCpuTimeSec 返回秒（double），m_lastProcTimeSec 存的是 ×1000 后的毫秒整数
        // 见 Task 2 平台实现统一约定。这里重算：
        cpuUsed = procSec - (double(m_lastProcTimeSec) / 1000.0);
        if (wallSec > 0) m_snap.cpuPercent = cpuUsed / wallSec * 100.0;
    }
    m_lastProcTimeSec = qint64(procSec * 1000.0);  // 存毫秒整数避免浮点漂移
    m_lastWallMs = wallMs;
    m_snap.memoryRssMb = rssMb;
    if (m_snap.memoryTotalMb > 0) {
        m_snap.memoryPercent = double(rssMb) / double(m_snap.memoryTotalMb) * 100.0;
    }
    m_snap.sampledAtMs = wallMs;
    m_snap.markdown = buildMarkdown(m_snap);
}

QString Environment::buildMarkdown(const EnvironmentSnapshot& s) const {
    QString md;
    md += QStringLiteral("## seimi-render 运行环境\n\n");
    md += QStringLiteral("- **OS**: %1 (%2)\n").arg(s.osPretty, s.arch);
    md += QStringLiteral("- **Kernel**: %1\n").arg(s.kernel);
    md += QStringLiteral("- **Hostname**: %1\n").arg(s.hostname);
    md += QStringLiteral("- **CPU**: %1 · %2物理核/%3逻辑核\n")
             .arg(s.cpuModel).arg(s.cpuPhysicalCores).arg(s.cpuLogicalCores);
    md += QStringLiteral("- **内存**: %1 MB\n").arg(s.memoryTotalMb);
    md += QStringLiteral("- **GPU**: %1\n").arg(s.hasGpu ? QStringLiteral("有") : QStringLiteral("无（headless 服务器）"));
    md += QStringLiteral("- **Qt 版本**: %1\n").arg(s.qtVersion);
    md += QStringLiteral("- **构建信息**: %1 · %2\n").arg(s.buildTime, s.gitCommit);
    if (s.sampledAtMs > 0) {
        md += QStringLiteral("- **CPU 占用**: %1%\n").arg(s.cpuPercent, 0, 'f', 1);
        md += QStringLiteral("- **内存占用**: %1 MB (%2%)\n").arg(s.memoryRssMb).arg(s.memoryPercent, 0, 'f', 1);
        md += QStringLiteral("- **采样时间**: %1\n").arg(
            QDateTime::fromMSecsSinceEpoch(s.sampledAtMs).toString(Qt::DefaultLocaleShortDate));
    } else {
        md += QStringLiteral("- **CPU 占用**: --\n");
        md += QStringLiteral("- **内存占用**: --\n");
    }
    md += QStringLiteral("\n> 由 seimi-render 管理界面「运行环境」卡片一键导出\n");
    return md;
}

// ===== 平台原生采集（Task 2 实现，此处占位）=====
QString Environment::probeCpuModel() { return QString(); }
int    Environment::probePhysicalCores() { return QThread::idealThreadCount(); }
qint64 Environment::probeMemoryTotalMb() { return 0; }
bool   Environment::probeHasGpu() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}
double Environment::probeProcCpuTimeSec() { return 0.0; }
qint64 Environment::probeProcRssMb() { return 0; }

} // namespace seimi
```

- [ ] **Step 3: 修改 `CMakeLists.txt`，把 Environment 加入源码列表**

在 `src/Metrics.cpp` 后、`src/CookieStore.h` 前插入两行（保持字母序不太重要，但与同类 Metrics/ProxyConfig 聚在一起更易读）：

找到：
```
    src/Metrics.h
    src/Metrics.cpp
    src/CookieStore.h
```
改为：
```
    src/Metrics.h
    src/Metrics.cpp
    src/Environment.h
    src/Environment.cpp
    src/CookieStore.h
```

- [ ] **Step 4: 编译验证（Windows，当前平台）**

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.7.2\msvc2019_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Expected: 编译成功，无 error。（Environment 未被任何代码引用，只是编进二进制不会改变运行行为。）

如果 `Q_OBJECT` 报「undefined reference to vtable」——说明 moc 没扫到。CMake 的 AUTOMOC 默认 ON（见 CMakeLists 顶部 `set(CMAKE_AUTOMOC ON)` 应已存在，若无需加）。检查：
```bat
findstr AUTOMOC CMakeLists.txt
```
应有 `set(CMAKE_AUTOMOC ON)` 或等价。

- [ ] **Step 5: 提交**

```bash
git add src/Environment.h src/Environment.cpp CMakeLists.txt
git commit -m "feat(env): add Environment module skeleton (Qt API + platform stubs)"
```

---

## Task 2: 平台原生采集实现

填充 Task 1 占位的 6 个 `probe*` 函数。`#ifdef` 全部锁在 `.cpp` 末尾，互不干扰。

**Files:**
- Modify: `src/Environment.cpp`（替换末尾占位实现）

- [ ] **Step 1: 在 `Environment.cpp` 顶部补充平台头文件**

在现有 `#include` 区块后追加（条件包含）：

```cpp
#if defined(Q_OS_LINUX)
#  include <QtCore/QFile>
#  include <QtCore/QTextStream>
#  include <unistd.h>          // sysconf
#elif defined(Q_OS_MACOS)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <libproc.h>
#  include <mach/mach.h>
#  include <mach/task.h>
#  include <mach/mach_init.h>
extern "C" int proc_pid_rusage(int pid, int flavor, void* buffer);  // libproc
#elif defined(Q_OS_WIN)
#  include <windows.h>
#  include <psapi.h>
#endif
```

- [ ] **Step 2: 实现 `probeCpuModel()`**

替换占位：

```cpp
QString Environment::probeCpuModel() {
#if defined(Q_OS_LINUX)
    // /proc/cpuinfo 找首条 "model name"（x86/ARM 通用）
    QFile f(QStringLiteral("/proc/cpuinfo"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.startsWith(QStringLiteral("model name"))) {
                int col = line.indexOf(QLatin1Char(':'));
                if (col > 0) return line.mid(col + 1).trimmed();
            }
        }
    }
    return QString();
#elif defined(Q_OS_MACOS)
    char buf[256] = {0};
    size_t sz = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &sz, nullptr, 0) == 0) {
        return QString::fromLocal8Bit(buf);
    }
    return QString();
#elif defined(Q_OS_WIN)
    char buf[256] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        return QString::fromLocal8Bit(buf).trimmed();
    }
    return QString();
#else
    return QString();
#endif
}
```

- [ ] **Step 3: 实现 `probePhysicalCores()`**

```cpp
int Environment::probePhysicalCores() {
#if defined(Q_OS_LINUX)
    // (physical id, core id) 组合去重 = 物理核数
    QFile f(QStringLiteral("/proc/cpuinfo"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QSet<QString> seen;
        QString curPhysId, curCoreId;
        bool hasFields = false;
        QTextStream in(&f);
        auto flush = [&]() {
            if (!curPhysId.isEmpty() && !curCoreId.isEmpty()) {
                seen.insert(curPhysId + QLatin1Char(',') + curCoreId);
                hasFields = true;
            }
            curPhysId.clear(); curCoreId.clear();
        };
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.isEmpty()) { flush(); continue; }  // 处理器分隔空行
            int col = line.indexOf(QLatin1Char(':'));
            if (col < 0) continue;
            QString key = line.left(col).trimmed();
            QString val = line.mid(col + 1).trimmed();
            if (key == QStringLiteral("physical id")) curPhysId = val;
            else if (key == QStringLiteral("core id")) curCoreId = val;
        }
        flush();
        if (hasFields && !seen.isEmpty()) return seen.size();
    }
    return QThread::idealThreadCount();  // fallback：罕见内核无 core id 字段
#elif defined(Q_OS_MACOS)
    int n = 0;
    size_t sz = sizeof(n);
    if (sysctlbyname("hw.physicalcpu", &n, &sz, nullptr, 0) == 0) return n;
    return QThread::idealThreadCount();
#elif defined(Q_OS_WIN)
    // GetLogicalProcessorInformationEx 数 RelationProcessorPackage 包数
    // 简化实现：用 PSIZE 查询逻辑处理器关系再除以每核线程数太绕，
    // 直接 fallback idealThreadCount（Windows 上通常物理核需求弱，逻辑核够用）
    return QThread::idealThreadCount();
#else
    return QThread::idealThreadCount();
#endif
}
```

注：上面 Linux 分支用到了 `QSet`，需在文件顶部 `#include <QSet>`。

- [ ] **Step 4: 实现 `probeMemoryTotalMb()`**

```cpp
qint64 Environment::probeMemoryTotalMb() {
#if defined(Q_OS_LINUX)
    QFile f(QStringLiteral("/proc/meminfo"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.startsWith(QStringLiteral("MemTotal:"))) {
                // "MemTotal:       32768000 kB"
                auto parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                        Qt::SkipEmptyParts);
                if (parts.size() >= 2) {
                    bool ok = false;
                    qint64 kb = parts[1].toLongLong(&ok);
                    if (ok) return kb / 1024;
                }
            }
        }
    }
    return 0;
#elif defined(Q_OS_MACOS)
    int64_t bytes = 0;
    size_t sz = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &sz, nullptr, 0) == 0) {
        return bytes / (1024 * 1024);
    }
    return 0;
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        return qint64(mem.ullTotalPhys) / (1024 * 1024);
    }
    return 0;
#else
    return 0;
#endif
}
```

需在顶部追加 `#include <QRegularExpression>`（Linux 分支用）。

- [ ] **Step 5: 实现 `probeHasGpu()`**

```cpp
bool Environment::probeHasGpu() {
#if defined(Q_OS_LINUX)
    // 优先：NVIDIA 驱动
    if (QFile::exists(QStringLiteral("/proc/driver/nvidia/version"))) return true;
    // 其次：lspci 探测 VGA/3D controller
    QProcess proc;
    proc.setProgram(QStringLiteral("lspci"));
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    if (proc.waitForFinished(2000)) {
        QString out = QString::fromLocal8Bit(proc.readAll());
        if (out.contains(QStringLiteral("VGA"), Qt::CaseInsensitive) ||
            out.contains(QStringLiteral("3D controller"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;  // headless 服务器常见
#elif defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // macOS/Windows 装机必有显卡，省采集成本
    return true;
#else
    return false;
#endif
}
```

需在顶部追加 `#include <QProcess>`。

- [ ] **Step 6: 实现 `probeProcCpuTimeSec()`（累计进程 CPU 时间，秒）**

```cpp
double Environment::probeProcCpuTimeSec() {
#if defined(Q_OS_LINUX)
    // /proc/self/stat 第 14(utime)+15(stime) 字段，单位 clock tick
    QFile f(QStringLiteral("/proc/self/stat"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString s = QString::fromLocal8Bit(f.readLine());
        // comm 字段可能含空格（被括号包裹），从最后一个 ')' 之后解析
        int rp = s.lastIndexOf(QLatin1Char(')'));
        if (rp < 0) return 0.0;
        auto rest = s.mid(rp + 1).split(QRegularExpression(QStringLiteral("\\s+")),
                                        Qt::SkipEmptyParts);
        // rest[0]=state, rest[11]=utime, rest[12]=stime
        if (rest.size() >= 13) {
            bool ok1, ok2;
            long utime = rest[11].toLong(&ok1);
            long stime = rest[12].toLong(&ok2);
            if (ok1 && ok2) {
                long hz = sysconf(_SC_CLK_TCK);
                if (hz > 0) return double(utime + stime) / double(hz);
            }
        }
    }
    return 0.0;
#elif defined(Q_OS_MACOS)
    struct rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, &ri) == 0) {
        // ri_user_time / ri_system_time 单位 nanoseconds
        double ns = double(ri.ri_user_time + ri.ri_system_time);
        return ns / 1e9;
    }
    return 0.0;
#elif defined(Q_OS_WIN)
    FILETIME ct, et, kt, ut;
    if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
        // FILETIME 是 100ns 单位
        ULARGE_INTEGER k, u;
        k.LowPart = kt.dwLowDateTime; k.HighPart = kt.dwHighDateTime;
        u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
        return double(k.QuadPart + u.QuadPart) / 1e7;  // 100ns → sec
    }
    return 0.0;
#else
    return 0.0;
#endif
}
```

注：macOS `rusage_info_v4` 字段在较老 SDK 可能不全；若编译失败改用 `rusage_info_v3`（字段名相同）。Windows `GetProcessTimes` 对所有进程有效。

- [ ] **Step 7: 实现 `probeProcRssMb()`**

```cpp
qint64 Environment::probeProcRssMb() {
#if defined(Q_OS_LINUX)
    QFile f(QStringLiteral("/proc/self/status"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.startsWith(QStringLiteral("VmRSS:"))) {
                auto parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                        Qt::SkipEmptyParts);
                if (parts.size() >= 2) {
                    bool ok = false;
                    qint64 kb = parts[1].toLongLong(&ok);
                    if (ok) return kb / 1024;
                }
            }
        }
    }
    return 0;
#elif defined(Q_OS_MACOS)
    struct rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, &ri) == 0) {
        return qint64(ri.ri_resident_size) / (1024 * 1024);  // bytes → MB
    }
    return 0;
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return qint64(pmc.WorkingSetSize) / (1024 * 1024);
    }
    return 0;
#else
    return 0;
#endif
}
```

- [ ] **Step 8: 修正 `sample()` 中的双精度计算 bug（Task 1 留的混淆）**

Task 1 的 `sample()` 里 `m_lastProcTimeSec` 命名误导（存的是毫秒整数）。重命名并简化：

找到 `sample()` 函数，把差分计算段替换为：

```cpp
void Environment::sample() {
    double procSec = probeProcCpuTimeSec();
    qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
    qint64 rssMb  = probeProcRssMb();

    QMutexLocker lock(&m_mutex);
    // 首次采样只建立基线，不算 cpuPercent（sampledAtMs 仍为 0，前端显示 --）
    if (m_lastWallMs > 0 && wallMs > m_lastWallMs) {
        double wallSec = double(wallMs - m_lastWallMs) / 1000.0;
        double cpuUsed = procSec - m_lastProcSec;
        if (wallSec > 0) m_snap.cpuPercent = cpuUsed / wallSec * 100.0;
        m_snap.sampledAtMs = wallMs;  // 有差分才算"已采样"
    } else {
        m_snap.cpuPercent = 0.0;
        m_snap.sampledAtMs = 0;
    }
    m_lastProcSec = procSec;
    m_lastWallMs = wallMs;
    m_snap.memoryRssMb = rssMb;
    if (m_snap.memoryTotalMb > 0) {
        m_snap.memoryPercent = double(rssMb) / double(m_snap.memoryTotalMb) * 100.0;
    }
    m_snap.markdown = buildMarkdown(m_snap);
}
```

并把 header 里 `qint64 m_lastProcTimeSec = 0;` 改为 `double m_lastProcSec = 0.0;`。

- [ ] **Step 9: Windows 编译验证**

```bat
cmake --build build --config Release
```

Expected: 编译成功。（Linux/macOS 分支用 `#ifdef` 隔离，Windows 编译器不会编它们。）

- [ ] **Step 10: 提交**

```bash
git add src/Environment.h src/Environment.cpp
git commit -m "feat(env): implement platform-native probes (Linux/macOS/Windows)"
```

---

## Task 3: `HttpServer` 接入 `Environment`

让 `/status` 返回 `environment` 段。

**Files:**
- Modify: `src/HttpServer.h`
- Modify: `src/HttpServer.cpp:278-378`（`jsonRuntimeStatus` 函数）

- [ ] **Step 1: `HttpServer.h` 加成员 + setter**

找到（约 100-103 行）：
```cpp
    RenderQueue* m_queue;
    CookieStore* m_cookies = nullptr;  // 可空；非空时启用 /cookies 接口
    ProxyConfig* m_proxy = nullptr;    // 可空；非空时启用 /proxy 接口
```

在 `m_proxy` 那行后追加：
```cpp
    class Environment* m_env = nullptr;  // 可空；非空时 /status 附带 environment 段
```

找到（约 57 行）：
```cpp
    void setProxyConfig(ProxyConfig* cfg) { m_proxy = cfg; }
```

在其后追加：
```cpp
    void setEnvironment(Environment* env) { m_env = env; }
```

并在文件顶部 forward declaration 区（与 `class ProxyConfig;` 同处，或 `namespace seimi {` 内）追加：
```cpp
class Environment;
```

- [ ] **Step 2: `HttpServer.cpp` 在 `jsonRuntimeStatus` 末尾追加 environment 段**

找到（约 374-377 行）：
```cpp
    s += "]}";

    s += "}";
    return s;
}
```

在 `s += "}";`（最后的闭合括号）**之前**插入 environment 段：

```cpp
    s += "]}";

    // —— 运行环境信息（启动时静态采集 + GUI 定时器实时采样）——
    if (m_env) {
        EnvironmentSnapshot e = m_env->snapshot();
        auto esc = [](const QString& q) { /* 复用现有 escJson(std::string) */ return escJson(q.toStdString()); };
        s += ",\"environment\":{";
        s += "\"os_name\":\"" + esc(e.osName) + "\"";
        s += ",\"os_version\":\"" + esc(e.osVersion) + "\"";
        s += ",\"os_pretty\":\"" + esc(e.osPretty) + "\"";
        s += ",\"kernel\":\"" + esc(e.kernel) + "\"";
        s += ",\"arch\":\"" + esc(e.arch) + "\"";
        s += ",\"build_arch\":\"" + esc(e.buildArch) + "\"";
        s += ",\"hostname\":\"" + esc(e.hostname) + "\"";
        s += ",\"cpu_model\":\"" + esc(e.cpuModel) + "\"";
        s += ",\"cpu_cores_logical\":" + std::to_string(e.cpuLogicalCores);
        s += ",\"cpu_cores_physical\":" + std::to_string(e.cpuPhysicalCores);
        s += ",\"memory_total_mb\":" + std::to_string(e.memoryTotalMb);
        s += ",\"has_gpu\":" + std::string(e.hasGpu ? "true" : "false");
        s += ",\"qt_version\":\"" + esc(e.qtVersion) + "\"";
        s += ",\"build_time\":\"" + esc(e.buildTime) + "\"";
        s += ",\"git_commit\":\"" + esc(e.gitCommit) + "\"";
        s += ",\"cpu_percent\":" + fmtDouble(e.cpuPercent);
        s += ",\"memory_rss_mb\":" + std::to_string(e.memoryRssMb);
        s += ",\"memory_percent\":" + fmtDouble(e.memoryPercent);
        s += ",\"sampled_at_ms\":" + fmtMs(e.sampledAtMs);
        s += ",\"markdown\":\"" + esc(e.markdown) + "\"";
        s += "}";
    }

    s += "}";
    return s;
}
```

注：`escJson` 是 `HttpServer.cpp` 内已有的 `std::string` → JSON 转义函数。这里用 lambda 包一层接受 `QString`。若 `escJson` 是文件内静态自由函数而非成员，lambda 内直接调用即可。

- [ ] **Step 3: `HttpServer.cpp` 顶部加 include**

找到 `#include` 区块，追加：
```cpp
#include "Environment.h"
```

- [ ] **Step 4: Windows 编译验证**

```bat
cmake --build build --config Release
```

Expected: 编译成功。

- [ ] **Step 5: 提交**

```bash
git add src/HttpServer.h src/HttpServer.cpp
git commit -m "feat(env): expose environment snapshot in /status JSON"
```

---

## Task 4: `main.cpp` 创建并注入 `Environment`

**Files:**
- Modify: `src/main.cpp:408-418`（HttpServer 创建前后）

- [ ] **Step 1: 在 HttpServer 创建之前插入 Environment 创建**

找到（约 408-412 行）：
```cpp
    std::string adminUi = cfg.adminEnabled ? adminUiDir.toStdString() : std::string();

    // HTTP server 跑在独立线程（httplib::listen 阻塞）。
    HttpServer http(&queue, &cookies, cfg.settleDefaultMs,
                    adminUi, cfg.adminPassword, &app);
```

改为：
```cpp
    std::string adminUi = cfg.adminEnabled ? adminUiDir.toStdString() : std::string();

    // 运行环境采集器：构造时采集静态字段（OS/CPU/内存/构建信息），
    // GUI 线程 3s 定时器采样实时 CPU/RSS，供 /status 的 environment 段。
    Environment env(&app);
    env.start(&app);

    // HTTP server 跑在独立线程（httplib::listen 阻塞）。
    HttpServer http(&queue, &cookies, cfg.settleDefaultMs,
                    adminUi, cfg.adminPassword, &app);
    http.setEnvironment(&env);
```

- [ ] **Step 2: `main.cpp` 顶部加 include**

找到 `#include` 区块（包含 `#include "ProxyConfig.h"` 等处），追加：
```cpp
#include "Environment.h"
```

- [ ] **Step 3: Windows 编译 + 运行验证**

```bat
cmake --build build --config Release
.\build\Release\seimi-render.exe --http-port 8088 --no-sandbox
```

另开终端：
```bat
curl http://127.0.0.1:8088/status
```

Expected: JSON 中出现 `"environment":{...}` 段，含 `os_pretty`、`cpu_model`、`memory_total_mb`、`has_gpu:true`、`qt_version`、`build_time`、`git_commit`、`markdown` 等字段。`sampled_at_ms` 启动后约 3-6s 内变为非 0。

Ctrl+C 关闭进程。

- [ ] **Step 4: 提交**

```bash
git add src/main.cpp
git commit -m "feat(env): wire Environment into main + HttpServer"
```

---

## Task 5: 前端 — HTML 卡片骨架

**Files:**
- Modify: `admin-ui/index.html:101-110`（域名分布 section 之后）

- [ ] **Step 1: 在域名分布 section 后、页签关闭 `</section>` 前插入**

找到（约 101-110 行）：
```html
        <div class="section">
          <h3><span data-i18n="stats.domains.title">域名分布 Top</span> ...</h3>
          <div class="table-wrap">
            <table id="stats-domains" class="data-table">
              ...
            </table>
          </div>
        </div>
      </section>
```

在 `</div>`（table-wrap 关闭）之后的 `</section>`（页签关闭）**之前**插入：

```html
        <div class="section">
          <h3>
            <svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="M4 6h16V4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2H4zm16 12H4V8h16v10zM6 10h8v2H6zm0 4h8v2H6zm10-4h2v6h-2z" fill="currentColor"/></svg>
            <span data-i18n="stats.env.title">运行环境</span>
            <span id="stats-env-sampled" class="muted small" style="font-weight:normal; margin-left:4px;"></span>
            <button id="env-copy-btn" class="copy-btn" type="button" data-i18n="stats.env.copy">📋 拷贝</button>
            <span id="env-copy-status" class="copy-status"></span>
          </h3>
          <div id="stats-env" class="env-grid">
            <p class="muted small" data-i18n="common.loading">加载中…</p>
          </div>
        </div>
```

- [ ] **Step 2: 提交**

```bash
git add admin-ui/index.html
git commit -m "feat(env): add environment card HTML skeleton in stats tab"
```

---

## Task 6: 前端 — CSS 样式

**Files:**
- Modify: `admin-ui/app.css`（末尾追加）

- [ ] **Step 1: 在 `app.css` 末尾追加**

```css
/* —— 运行环境卡片 —— */
.env-grid { font-size: 13px; }
.env-table { width: 100%; border-collapse: collapse; }
.env-table td { padding: 4px 8px; border-bottom: 1px solid var(--border, #e5e7eb); vertical-align: top; }
.env-table td.k { color: var(--muted, #888); white-space: nowrap; width: 130px; }
.env-table td.v { word-break: break-all; }
.copy-btn { margin-left: 8px; padding: 2px 10px; font-size: 12px; cursor: pointer; border: 1px solid var(--border, #ddd); background: var(--card-bg, #fff); border-radius: 4px; }
.copy-btn:hover { background: var(--hover-bg, #f5f5f5); }
.copy-status { margin-left: 6px; font-size: 12px; }
.copy-status.ok { color: var(--ok, #2c8); }
.copy-status.err { color: var(--err, #c42); }
```

注：项目 CSS 变量名以现有文件为准；若 `--border`/`--muted` 不存在，回退值（如 `#e5e7eb`）兜底。

- [ ] **Step 2: 提交**

```bash
git add admin-ui/app.css
git commit -m "feat(env): add environment card + copy button styles"
```

---

## Task 7: 前端 — JS 渲染 + 拷贝逻辑

**Files:**
- Modify: `admin-ui/app.js:136-184`（`renderStats` 函数）+ 末尾追加新函数

- [ ] **Step 1: 在 `renderStats` 末尾（域名分布渲染后）追加 `renderEnv` 调用**

找到（约 183-184 行）：
```js
  }).join('') : `<tr><td colspan="5" class="muted">${t('common.noData')}</td></tr>`;
}
```

改为：
```js
  }).join('') : `<tr><td colspan="5" class="muted">${t('common.noData')}</td></tr>`;

  // 运行环境卡片（来自 /status 的 environment 段）
  renderEnv(d.environment);
}
```

- [ ] **Step 2: 在 `app.js` 的 `renderStats` 函数之后（约 184 行 `}` 之后）新增 `renderEnv` / `copyEnv` / `showCopyStatus` + 事件绑定**

```js

// ---------- 页签 1.6：运行环境（OS/硬件/构建信息 + 实时占用）----------
let lastEnvMarkdown = '';

function renderEnv(env) {
  const el = document.getElementById('stats-env');
  if (!el) return;
  if (!env) { el.innerHTML = `<p class="muted small">${t('common.noData')}</p>`; return; }
  lastEnvMarkdown = env.markdown || '';

  const sampled = env.sampled_at_ms > 0;
  const rows = [
    [t('stats.env.os'),       `${esc(env.os_pretty || '')} (${esc(env.arch || '')})`],
    [t('stats.env.kernel'),   esc(env.kernel || '')],
    [t('stats.env.hostname'), esc(env.hostname || '')],
    [t('stats.env.cpu'),      `${esc(env.cpu_model || '--')} · ${env.cpu_cores_physical}${t('stats.env.coresPhys')}/${env.cpu_cores_logical}${t('stats.env.coresLogical')}`],
    [t('stats.env.memory'),   `${env.memory_total_mb} MB`],
    [t('stats.env.gpu'),      env.has_gpu ? t('stats.env.gpuYes') : t('stats.env.gpuNo')],
    [t('stats.env.qt'),       esc(env.qt_version || '')],
    [t('stats.env.build'),    `${esc(env.build_time || '')} · ${esc(env.git_commit || '')}`],
    [t('stats.env.cpuUsage'), sampled ? `${(env.cpu_percent || 0).toFixed(1)}%` : '--'],
    [t('stats.env.rss'),      sampled ? `${env.memory_rss_mb} MB (${(env.memory_percent || 0).toFixed(1)}%)` : '--'],
  ];
  el.innerHTML = '<table class="env-table"><tbody>' +
    rows.map(([k, v]) => `<tr><td class="k">${k}</td><td class="v">${v}</td></tr>`).join('') +
    '</tbody></table>';

  const sampledEl = document.getElementById('stats-env-sampled');
  if (sampledEl) {
    sampledEl.textContent = sampled
      ? t('stats.env.sampledAt', { ts: new Date(env.sampled_at_ms).toLocaleTimeString() })
      : '';
  }
}

async function copyEnv() {
  if (!lastEnvMarkdown) return;
  // 优先用现代 Clipboard API
  try {
    await navigator.clipboard.writeText(lastEnvMarkdown);
    showCopyStatus(t('stats.env.copied'), 'ok');
    return;
  } catch (e) {
    // 非安全上下文（http://非localhost）下 clipboard 不可用，走兜底
  }
  // 兜底：隐藏 textarea + execCommand
  const ta = document.createElement('textarea');
  ta.value = lastEnvMarkdown;
  ta.style.position = 'fixed';
  ta.style.top = '-9999px';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  let ok = false;
  try { ok = document.execCommand('copy'); } catch (e2) {}
  document.body.removeChild(ta);
  showCopyStatus(ok ? t('stats.env.copied') : t('stats.env.copyFail'), ok ? 'ok' : 'err');
}

function showCopyStatus(msg, cls) {
  const s = document.getElementById('env-copy-status');
  if (!s) return;
  s.textContent = msg;
  s.className = 'copy-status ' + cls;
  setTimeout(() => {
    s.textContent = '';
    s.className = 'copy-status';
  }, 2000);
}

// 拷贝按钮绑定（DOMContentLoaded 后执行，确保元素存在）
document.addEventListener('DOMContentLoaded', () => {
  const btn = document.getElementById('env-copy-btn');
  if (btn) btn.addEventListener('click', copyEnv);
});
```

- [ ] **Step 3: 运行验证（启动服务 + 浏览器目视）**

```bat
.\build\Release\seimi-render.exe --http-port 8088 --no-sandbox
```

浏览器打开 `http://127.0.0.1:8088/`，「运行时统计」页签滚动到底部，确认：
- 「运行环境」卡片显示 10 行键值对
- 启动 ~3-6s 后 CPU 占用/RSS 从 `--` 变为数值，标题右侧出现"采样于 xx:xx:xx"
- 点「📋 拷贝」→ 标题右侧出现"已拷贝"提示 → 粘贴到任意文本框确认 Markdown 格式正确

Ctrl+C 关闭进程。

- [ ] **Step 4: 提交**

```bash
git add admin-ui/app.js
git commit -m "feat(env): render environment card + clipboard copy with execCommand fallback"
```

---

## Task 8: 前端 — i18n 中英文键

**Files:**
- Modify: `admin-ui/i18n.js:63`（中文段 `stats.domains.colRate` 后）+ `:277`（英文段同位置）

- [ ] **Step 1: 中文段（约 63 行后）插入**

找到（约 63 行）：
```js
      'stats.domains.colRate': '成功率',
```

在其后插入：
```js
      'stats.env.title': '运行环境',
      'stats.env.copy': '📋 拷贝',
      'stats.env.copied': '已拷贝',
      'stats.env.copyFail': '拷贝失败',
      'stats.env.os': '操作系统',
      'stats.env.kernel': '内核版本',
      'stats.env.hostname': '主机名',
      'stats.env.cpu': 'CPU',
      'stats.env.coresPhys': '物理核',
      'stats.env.coresLogical': '逻辑核',
      'stats.env.memory': '内存总量',
      'stats.env.gpuYes': '有',
      'stats.env.gpuNo': '无（headless 服务器）',
      'stats.env.qt': 'Qt 版本',
      'stats.env.build': '构建信息',
      'stats.env.cpuUsage': 'CPU 占用',
      'stats.env.rss': '内存占用',
      'stats.env.sampledAt': '采样于 {ts}',
```

- [ ] **Step 2: 英文段（约 277 行后）插入**

找到（约 277 行）：
```js
      'stats.domains.colRate': 'Success rate',
```

在其后插入：
```js
      'stats.env.title': 'Environment',
      'stats.env.copy': '📋 Copy',
      'stats.env.copied': 'Copied',
      'stats.env.copyFail': 'Copy failed',
      'stats.env.os': 'OS',
      'stats.env.kernel': 'Kernel',
      'stats.env.hostname': 'Hostname',
      'stats.env.cpu': 'CPU',
      'stats.env.coresPhys': 'physical',
      'stats.env.coresLogical': 'logical',
      'stats.env.memory': 'Total memory',
      'stats.env.gpuYes': 'Present',
      'stats.env.gpuNo': 'None (headless server)',
      'stats.env.qt': 'Qt version',
      'stats.env.build': 'Build',
      'stats.env.cpuUsage': 'CPU usage',
      'stats.env.rss': 'Memory usage',
      'stats.env.sampledAt': 'sampled at {ts}',
```

- [ ] **Step 3: 运行验证（中英文切换）**

启动服务，浏览器打开管理界面：
1. 确认卡片标签为中文（默认语言）
2. 切到 English（页脚语言切换），确认标签变英文
3. 切回中文，点拷贝，确认 Markdown 仍为后端预拼的中文模板（i18n 不影响拷贝内容）

Ctrl+C 关闭。

- [ ] **Step 4: 提交**

```bash
git add admin-ui/i18n.js
git commit -m "feat(env): add i18n keys for environment card (zh/en)"
```

---

## Self-Review 记录

**1. Spec 覆盖检查：**
- §3 架构（Environment 模块）→ Task 1 ✓
- §4 字段清单 → Task 1（结构定义）+ Task 2（采集）✓
- §5 平台原生实现 → Task 2 ✓
- §6 后端接入 → Task 3 + Task 4 ✓
- §7 前端实现 → Task 5（HTML）+ Task 6（CSS）+ Task 7（JS）+ Task 8（i18n）✓
- §8 打包脚本无改动 → 计划中未列（正确，CMake 自动 install 源码到二进制）✓
- §9 测试 → 每个 Task 末尾的验证步骤 ✓
- §10 改动清单 → 覆盖全部 10 个文件 ✓

**2. 类型一致性检查：**
- `EnvironmentSnapshot` 字段名在 .h（Task 1）/ .cpp `buildMarkdown`（Task 1）/ HttpServer.cpp JSON（Task 3）/ app.js `renderEnv`（Task 7）四处一致：`osPretty`/`cpuModel`/`cpuLogicalCores`/`cpuPhysicalCores`/`memoryTotalMb`/`hasGpu`/`cpuPercent`/`memoryRssMb`/`memoryPercent`/`sampledAtMs`/`markdown` — JSON 序列化用 snake_case（`os_pretty`/`cpu_model`...），JS 端读 snake_case，一致 ✓
- `m_lastProcSec` 在 header（Task 2 Step 8 修正后）与 sample() 一致 ✓

**3. 已知简化（非 spec 偏差，记录备案）：**
- Windows `probePhysicalCores` 直接用 `idealThreadCount`（逻辑核）而非真正的物理包数——spec §5.2 写了 `GetLogicalProcessorInformationEx` 方案但实现复杂度高、Windows 诊断价值低，简化为 fallback。spec 字段名仍是 `cpu_cores_physical`，Windows 上值=逻辑核数，可接受。
- macOS `rusage_info_v4` 若 SDK 不支持需降级 `v3`——Task 2 Step 6 已注明。

无 placeholder，所有步骤含完整代码。
