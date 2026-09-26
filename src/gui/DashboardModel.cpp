// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "DashboardModel.h"
#include "GuiPrefs.h"

#include <QDateTime>

namespace seimi {
namespace {

QString msToUptime(qint64 ms) {
    const qint64 totalSec = ms / 1000;
    const qint64 days = totalSec / 86400;
    const qint64 hours = (totalSec % 86400) / 3600;
    const qint64 mins = (totalSec % 3600) / 60;
    if (days > 0) return QStringLiteral("%1d %2h").arg(days).arg(hours);
    if (hours > 0) return QStringLiteral("%1h %2m").arg(hours).arg(mins);
    return QStringLiteral("%1m").arg(mins);
}

QString msLatency(double ms) {
    if (ms <= 0) return QStringLiteral("—");
    if (ms < 1000.0) return QString::number(ms, 'f', 0) + QStringLiteral(" ms");
    return QString::number(ms / 1000.0, 'f', 2) + QStringLiteral(" s");
}

} // namespace

DashboardModel::DashboardModel(const Deps& deps, QObject* parent)
    : QObject(parent)
    , m_deps(deps)
{
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &DashboardModel::tick);
    m_timer.start();
    tick();
}

void DashboardModel::refresh() { tick(); }

void DashboardModel::tick() {
    if (!m_deps.queue) return;

    // 顺带驱动「跟随系统深浅」的轮询探测（Qt 6.7 的 colorScheme 变更信号不可靠）。
    if (m_deps.prefs) m_deps.prefs->syncSystemDark();

    // 域名分布取较多条（概览默认 Top 8，「展开全部」最多展示 kDomainCap 条）；
    // 统计口径不变（终态任务数 = succeeded + failed，成功率分母 = total）。
    static constexpr int kDomainCap = 100;
    const Metrics::Snapshot m = m_deps.queue->metrics().snapshot(kDomainCap);
    const RenderQueue::Stats st = m_deps.queue->stats();

    m_uptimeText = msToUptime(m.uptimeMsec);
    m_total = int(m.total);
    m_succeeded = int(m.succeeded);
    m_failed = int(m.failed);
    m_successRateText = m.total > 0
        ? QStringLiteral("%1%").arg(QString::number(m.successRate * 100.0, 'f', 1))
        : QStringLiteral("—");
    m_throughputText = QString::number(m.throughputPerSec, 'f', 2)
        + QStringLiteral("/s");
    m_p50 = msLatency(double(m.latencyP50Ms));
    m_p90 = msLatency(double(m.latencyP90Ms));
    m_avg = msLatency(m.latencyAvgMs);

    m_pending = st.pending;
    m_running = st.running;
    m_busy = st.busy;
    m_workers = st.concurrency;
    m_peak = st.peakPending;

    if (m_deps.proxy) {
        const ProxySpec p = m_deps.proxy->snapshot();
        m_proxyActive = p.type != ProxySpec::Type::Direct;
        if (!m_proxyActive) {
            m_proxySummary = QStringLiteral("direct");
        } else {
            const char* t = p.type == ProxySpec::Type::Socks5 ? "socks5" : "http";
            m_proxySummary = QStringLiteral("%1://%2:%3%4")
                .arg(QLatin1String(t))
                .arg(QString::fromStdString(p.host))
                .arg(p.port)
                .arg(p.user.empty() ? QString() : QStringLiteral(" ·auth"));
        }
    }
    if (m_deps.cookies) {
        const CookieStore::Overview ov = m_deps.cookies->snapshot(200);
        m_cookieTotal = ov.total;
        m_cookieDomains = int(ov.domains.size());
    }
    if (m_deps.mcp) {
        m_mcpActive = m_deps.mcp->isRunning();
        m_mcpSessions = int(m_deps.mcp->activeSessionCount());
        m_mcpMax = int(m_deps.mcp->maxSessions());
    }
    if (m_deps.env) {
        const EnvironmentSnapshot e = m_deps.env->snapshot();
        m_cpu = QString::number(e.cpuPercent, 'f', 1) + QStringLiteral("%");
        m_rss = QString::number(e.memoryRssMb) + QStringLiteral(" MB");
        m_envTitle = QStringLiteral("%1 · %2").arg(e.osPretty, e.arch);
        m_envOs = e.osPretty;
        m_envArch = e.arch;
    }

    m_blockedTotal = int(m.blockedTotal);
    m_blockedRecovered = int(m.blockedRecovered);
    m_blockedExhausted = int(m.blockedExhausted);

    m_domainCount = int(m.distinctDomains);

    m_topDomains.clear();
    for (const Metrics::DomainStat& d : m.domains) {
        QVariantMap row;
        row.insert(QStringLiteral("domain"), QString::fromStdString(d.host));
        row.insert(QStringLiteral("total"), qint64(d.total));
        row.insert(QStringLiteral("succeeded"), qint64(d.succeeded));
        row.insert(QStringLiteral("failed"), qint64(d.failed));
        // 成功率分母 = 该域终态任务数 total（= succeeded + failed；blocked 是事件次数非任务数）。
        row.insert(QStringLiteral("rate"), d.total > 0
            ? double(d.succeeded) / double(d.total) : 0.0);
        m_topDomains.append(row);
    }

    emit changed();
}

// ===================== ThroughputModel =====================

ThroughputModel::ThroughputModel(RenderQueue* queue, QObject* parent)
    : QObject(parent)
    , m_queue(queue)
{
    m_completed.reserve(kWindowSamples);
    m_submitted.reserve(kWindowSamples);
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &ThroughputModel::tick);
    m_timer.start();
    // 首拍基线（不产生第一个点，避免 0/dt 噪声）。
    if (m_queue) {
        m_lastTotalTerminal = m_queue->metrics().snapshot(0).total;
        const RenderQueue::Stats st = m_queue->stats();
        m_lastTotalSubmitted = qint64(st.total) + st.evicted;
        m_lastMs = QDateTime::currentMSecsSinceEpoch();
    }
}

void ThroughputModel::tick() {
    if (!m_queue) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 dtMs = (m_lastMs > 0) ? now - m_lastMs : 0;
    if (dtMs <= 0) return;

    const qint64 terminal = m_queue->metrics().snapshot(0).total;
    const RenderQueue::Stats st = m_queue->stats();
    const qint64 submitted = qint64(st.total) + st.evicted;

    const double completedRate = double(terminal - m_lastTotalTerminal) * 1000.0 / double(dtMs);
    const double submittedRate = double(submitted - m_lastTotalSubmitted) * 1000.0 / double(dtMs);
    m_lastTotalTerminal = terminal;
    m_lastTotalSubmitted = submitted;
    m_lastMs = now;

    m_completed.append(completedRate);
    m_submitted.append(submittedRate);
    if (m_completed.size() > kWindowSamples) m_completed.remove(0);
    if (m_submitted.size() > kWindowSamples) m_submitted.remove(0);

    const auto fmt = [](double v) {
        return QString::number(v, 'f', v < 10 ? 1 : 0);
    };
    m_completedRateText = fmt(completedRate);
    m_submittedRateText = fmt(submittedRate);

    emit changed();
}

QVariantList ThroughputModel::completedSeries() const {
    QVariantList out;
    out.reserve(m_completed.size());
    for (double v : m_completed) out.append(v);
    return out;
}

QVariantList ThroughputModel::submittedSeries() const {
    QVariantList out;
    out.reserve(m_submitted.size());
    for (double v : m_submitted) out.append(v);
    return out;
}

} // namespace seimi
