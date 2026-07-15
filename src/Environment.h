// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#ifndef SEIMI_ENVIRONMENT_H
#define SEIMI_ENVIRONMENT_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <cstdint>

// 前向声明放全局命名空间（而非 namespace seimi 内），否则 `class QTimer` 会被解释成
// seimi::QTimer，污染同 namespace 内其它头文件对全局 ::QTimer 的引用（GCC 严格两阶段
// 名称查找会因此把 QTimer 当成不完整类型，MSVC 名称查找更宽松侥幸能过——这是真实跨平台坑）。
class QTimer;

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
// 线程模型：构造与 snapshot() 可在任意线程调（snapshot 自带锁）；
// start() 必须在 GUI 线程调（this 与 mainThreadParent 都须亲和 GUI 线程，否则
// QTimer 跨线程启动非法）。采样定时器跑在 qApp->thread()。
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
    double m_lastProcSec = 0.0;     // 上次采样的累计进程 CPU 时间（秒）
    qint64 m_lastWallMs = 0;        // 上次采样的墙钟时间戳
    QTimer* m_timer = nullptr;
};

} // namespace seimi

#endif // SEIMI_ENVIRONMENT_H
