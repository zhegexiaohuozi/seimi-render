#include "Environment.h"

#include <QDateTime>
#include <QLocale>
#include <QMutexLocker>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>

#if defined(Q_OS_LINUX)
#  include <QtCore/QFile>
#  include <QtCore/QTextStream>
#  include <QtCore/QRegularExpression>
#  include <QtCore/QSet>
#  include <QtCore/QProcess>
#  include <unistd.h>          // sysconf, read, close
#  include <fcntl.h>           // open, O_RDONLY, O_CLOEXEC
#elif defined(Q_OS_MACOS)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <unistd.h>          // getpid
// proc_pid_rusage 在 <libproc.h>，但该头在某些 SDK 版本可见性不稳定（依赖 _LIBPROC
// 等内部宏），这里显式声明原型更稳妥；避免与系统头混用产生原型漂移。
extern "C" int proc_pid_rusage(int pid, int flavor, void* buffer);
#elif defined(Q_OS_WIN)
#  include <windows.h>
#  include <psapi.h>
#endif

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
            QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(s.sampledAtMs), QLocale::ShortFormat));
    } else {
        md += QStringLiteral("- **CPU 占用**: --\n");
        md += QStringLiteral("- **内存占用**: --\n");
    }
    md += QStringLiteral("\n> 由 seimi-render 管理界面「运行环境」卡片一键导出\n");
    return md;
}

// ===== 平台原生采集实现（#ifdef 隔离，失败返回默认值）=====

#if defined(Q_OS_LINUX)
// 读 /proc 虚拟文件全文。/proc 文件 stat 报 size=0，Qt 的 QFile/QTextStream 会据此判定
// 文件为空（QTextStream::atEnd() 立即 true、QFile::readAll() 返回空），即使内容实际存在
// ——这是 Qt 读虚拟文件系统的已知坑，WSL/容器/真实 Linux 都中招。
// 必须绕开 Qt 缓冲层，直接用 POSIX open()/read() 按块读到 EOF。
// 失败返回空字符串。
static QString readProcAll(const char* path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return QString();
    QByteArray buf;
    char chunk[4096];
    while (true) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            buf.append(chunk, int(n));
        } else {
            break;  // n==0 EOF 或 n<0 错误都终止
        }
    }
    ::close(fd);
    return QString::fromLocal8Bit(buf);
}
#endif

QString Environment::probeCpuModel() {
#if defined(Q_OS_LINUX)
    // /proc/cpuinfo 找首条 "model name"（x86/ARM 通用）
    const QString content = readProcAll("/proc/cpuinfo");
    for (const QString& line : content.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("model name"))) {
            int col = line.indexOf(QLatin1Char(':'));
            if (col > 0) return line.mid(col + 1).trimmed();
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

int Environment::probePhysicalCores() {
#if defined(Q_OS_LINUX)
    // (physical id, core id) 组合去重 = 物理核数
    const QString content = readProcAll("/proc/cpuinfo");
    if (!content.isEmpty()) {
        QSet<QString> seen;
        QString curPhysId, curCoreId;
        bool hasFields = false;
        auto flush = [&]() {
            if (!curPhysId.isEmpty() && !curCoreId.isEmpty()) {
                seen.insert(curPhysId + QLatin1Char(',') + curCoreId);
                hasFields = true;
            }
            curPhysId.clear(); curCoreId.clear();
        };
        for (const QString& line : content.split(QLatin1Char('\n'))) {
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
    // Windows：GetLogicalProcessorInformationEx 数包数实现复杂、诊断价值低，
    // 直接用逻辑核数（Windows 部署通常不关心物理/逻辑区分）。
    return QThread::idealThreadCount();
#else
    return QThread::idealThreadCount();
#endif
}

qint64 Environment::probeMemoryTotalMb() {
#if defined(Q_OS_LINUX)
    const QString content = readProcAll("/proc/meminfo");
    for (const QString& line : content.split(QLatin1Char('\n'))) {
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

double Environment::probeProcCpuTimeSec() {
#if defined(Q_OS_LINUX)
    // /proc/self/stat 第 14(utime)+15(stime) 字段，单位 clock tick
    const QString s = readProcAll("/proc/self/stat");
    if (!s.isEmpty()) {
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

qint64 Environment::probeProcRssMb() {
#if defined(Q_OS_LINUX)
    const QString content = readProcAll("/proc/self/status");
    for (const QString& line : content.split(QLatin1Char('\n'))) {
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

} // namespace seimi
