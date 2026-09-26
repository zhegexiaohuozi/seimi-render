// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../CookieStore.h"
#include "../Environment.h"
#include "../Metrics.h"
#include "../McpServer.h"
#include "../ProxyConfig.h"
#include "../RenderQueue.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

namespace seimi {

class GuiPrefs;

// 概览页数据模型（QML 单例）：2s 定时器把线程安全快照搬进 Q_PROPERTY，
// QML 绑定自动刷新——QML 侧零轮询、零跨线程。
// 只读模型；写操作（代理等）在 SettingsModel。
class DashboardModel : public QObject {
    Q_OBJECT
    // —— KPI ——
    Q_PROPERTY(QString uptimeText READ uptimeText NOTIFY changed)
    Q_PROPERTY(int totalTasks READ totalTasks NOTIFY changed)
    Q_PROPERTY(int succeeded READ succeeded NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
    Q_PROPERTY(QString successRateText READ successRateText NOTIFY changed)
    Q_PROPERTY(QString throughputText READ throughputText NOTIFY changed)
    Q_PROPERTY(QString p50Text READ p50Text NOTIFY changed)
    Q_PROPERTY(QString p90Text READ p90Text NOTIFY changed)
    Q_PROPERTY(QString avgText READ avgText NOTIFY changed)
    // —— 负载 ——
    Q_PROPERTY(int queuePending READ queuePending NOTIFY changed)
    Q_PROPERTY(int queueRunning READ queueRunning NOTIFY changed)
    Q_PROPERTY(int workersBusy READ workersBusy NOTIFY changed)
    Q_PROPERTY(int workersTotal READ workersTotal NOTIFY changed)
    Q_PROPERTY(int peakPending READ peakPending NOTIFY changed)
    Q_PROPERTY(double workerUsage READ workerUsage NOTIFY changed) // 0..1（busy/total）
    // —— 摘要卡 ——
    Q_PROPERTY(QString proxySummary READ proxySummary NOTIFY changed)
    Q_PROPERTY(bool proxyActive READ proxyActive NOTIFY changed)
    Q_PROPERTY(int cookieTotal READ cookieTotal NOTIFY changed)
    Q_PROPERTY(int cookieDomains READ cookieDomains NOTIFY changed)
    Q_PROPERTY(bool mcpActive READ mcpActive NOTIFY changed)
    Q_PROPERTY(int mcpSessions READ mcpSessions NOTIFY changed)
    Q_PROPERTY(int mcpMax READ mcpMax NOTIFY changed)
    Q_PROPERTY(double mcpUsage READ mcpUsage NOTIFY changed)
    Q_PROPERTY(QString cpuText READ cpuText NOTIFY changed)
    Q_PROPERTY(QString rssText READ rssText NOTIFY changed)
    Q_PROPERTY(QString envTitle READ envTitle NOTIFY changed)
    Q_PROPERTY(QString envOs READ envOs NOTIFY changed)
    Q_PROPERTY(QString envArch READ envArch NOTIFY changed)
    // —— 反爬 ——
    Q_PROPERTY(int blockedTotal READ blockedTotal NOTIFY changed)
    Q_PROPERTY(int blockedRecovered READ blockedRecovered NOTIFY changed)
    Q_PROPERTY(int blockedExhausted READ blockedExhausted NOTIFY changed)
    // —— 域名 Top ——
    Q_PROPERTY(QVariantList topDomains READ topDomains NOTIFY changed)
    Q_PROPERTY(int domainCount READ domainCount NOTIFY changed)   // 去重域名总数（分页上限内为精确值）
public:
    struct Deps {
        RenderQueue* queue = nullptr;
        ProxyConfig* proxy = nullptr;
        CookieStore* cookies = nullptr;
        McpServer* mcp = nullptr;
        Environment* env = nullptr;
        GuiPrefs* prefs = nullptr; // 顺带驱动 syncSystemDark()
    };

    explicit DashboardModel(const Deps& deps, QObject* parent = nullptr);

    Q_INVOKABLE void refresh(); // 立即刷新一次（页面切入时可调）

    QString uptimeText() const { return m_uptimeText; }
    int totalTasks() const { return m_total; }
    int succeeded() const { return m_succeeded; }
    int failed() const { return m_failed; }
    QString successRateText() const { return m_successRateText; }
    QString throughputText() const { return m_throughputText; }
    QString p50Text() const { return m_p50; }
    QString p90Text() const { return m_p90; }
    QString avgText() const { return m_avg; }
    int queuePending() const { return m_pending; }
    int queueRunning() const { return m_running; }
    int workersBusy() const { return m_busy; }
    int workersTotal() const { return m_workers; }
    int peakPending() const { return m_peak; }
    double workerUsage() const { return m_workers > 0 ? double(m_busy) / m_workers : 0.0; }
    QString proxySummary() const { return m_proxySummary; }
    bool proxyActive() const { return m_proxyActive; }
    int cookieTotal() const { return m_cookieTotal; }
    int cookieDomains() const { return m_cookieDomains; }
    bool mcpActive() const { return m_mcpActive; }
    int mcpSessions() const { return m_mcpSessions; }
    int mcpMax() const { return m_mcpMax; }
    double mcpUsage() const { return m_mcpMax > 0 ? double(m_mcpSessions) / m_mcpMax : 0.0; }
    QString cpuText() const { return m_cpu; }
    QString rssText() const { return m_rss; }
    QString envTitle() const { return m_envTitle; }
    QString envOs() const { return m_envOs; }
    QString envArch() const { return m_envArch; }
    int blockedTotal() const { return m_blockedTotal; }
    int blockedRecovered() const { return m_blockedRecovered; }
    int blockedExhausted() const { return m_blockedExhausted; }
    QVariantList topDomains() const { return m_topDomains; }
    int domainCount() const { return m_domainCount; }

signals:
    void changed();

private:
    void tick();

    Deps m_deps;
    QTimer m_timer;

    QString m_uptimeText, m_successRateText, m_throughputText, m_p50, m_p90, m_avg;
    QString m_proxySummary, m_cpu, m_rss, m_envTitle, m_envOs, m_envArch;
    int m_total = 0, m_succeeded = 0, m_failed = 0;
    int m_pending = 0, m_running = 0, m_busy = 0, m_workers = 0, m_peak = 0;
    bool m_proxyActive = false;
    int m_cookieTotal = 0, m_cookieDomains = 0;
    bool m_mcpActive = false;
    int m_mcpSessions = 0, m_mcpMax = 0;
    int m_blockedTotal = 0, m_blockedRecovered = 0, m_blockedExhausted = 0;
    int m_domainCount = 0;
    QVariantList m_topDomains;
};

// 侧栏吞吐曲线：2s 环形采样（60s 窗口），completed（终态增速）与
// submitted（提交增速 = stats().total + evicted 的差分）双通道。
class ThroughputModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList completedSeries READ completedSeries NOTIFY changed)
    Q_PROPERTY(QVariantList submittedSeries READ submittedSeries NOTIFY changed)
    Q_PROPERTY(QString completedRateText READ completedRateText NOTIFY changed)
    Q_PROPERTY(QString submittedRateText READ submittedRateText NOTIFY changed)
public:
    explicit ThroughputModel(RenderQueue* queue, QObject* parent = nullptr);

    QVariantList completedSeries() const;
    QVariantList submittedSeries() const;
    QString completedRateText() const { return m_completedRateText; }
    QString submittedRateText() const { return m_submittedRateText; }

signals:
    void changed();

private:
    void tick();

    RenderQueue* m_queue;
    QTimer m_timer;
    QVector<double> m_completed;   // 归一化前的每秒速率
    QVector<double> m_submitted;
    qint64 m_lastTotalTerminal = 0;   // metrics.total 上次值
    qint64 m_lastTotalSubmitted = 0;  // stats.total + evicted 上次值
    qint64 m_lastMs = 0;
    QString m_completedRateText, m_submittedRateText;

    static constexpr int kWindowSamples = 30; // 30 × 2s = 60s
};

} // namespace seimi
