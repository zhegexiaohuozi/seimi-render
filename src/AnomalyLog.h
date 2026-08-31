// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QDate>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <cstdint>

namespace seimi {

// 请求异常记录日志：所有「请求级异常」逐条落盘为 JSONL，供离线统一分析
//（异常根因定位 / 站点成功率归因 / 下一阶段优化决策）。
//
// 记录范围（kind 字段区分）：
//   failed            —— 渲染终态失败（超时/网络不可达/反爬拦截耗尽/截图失败等全部失败路径）
//   blocked_recovered —— 渲染期间命中反爬拦截页但重试后成功（风控压力信号，影响站点成功率）
//   rejected          —— 入口直接拒绝（SSRF 拦截 / 队列过载背压 503），任务未建立
//
// 每条记录携带根因分析所需的完整上下文：URL/host、失败阶段分类（stage）、错误原文、
// 反爬命中/重试计数、排队与渲染分段耗时、终态时刻队列负载、当时代理状态（不含凭据）、
// 请求参数（settle/outputs）与构建版本。与 metrics.json（成功/失败聚合）互补：
// 本日志是失败明细，metrics 是全量比率，两者 join（按 host）即得站点成功率归因。
//
// 落盘：<dataDir>/anomalies/anomaly-YYYYMMDD.jsonl，按本地日期轮转；
// 超过保留期（kRetentionDays）的旧文件在轮转时清理。文件权限 0600
//（URL query 可能含凭据）。每行一个紧凑 JSON 对象（schema v1，字段见 .cpp）。
//
// 线程模型：任意线程可调 record*（RenderQueue 终态在 GUI 线程、SSRF 拒绝在 httplib 线程），
// 内部单 QMutex 串行化，文件句柄常开、逐条 flush。写失败只告警（每天至多一次）不抛错，
// 绝不影响渲染主路径。dataDir 为空 = 禁用（与 Metrics/CookieStore 同构）。
class AnomalyLog {
public:
    explicit AnomalyLog(const QString& dataDir = QString());
    ~AnomalyLog();

    AnomalyLog(const AnomalyLog&) = delete;
    AnomalyLog& operator=(const AnomalyLog&) = delete;

    // 渲染终态异常记录（RenderQueue::setTerminal 释锁后调用）。
    // kind="failed"（succeeded=false）或 "blocked_recovered"（成功但期间命中过拦截页）。
    struct RenderRecord {
        bool succeeded = false;          // 终态是否成功
        QString taskId;
        QString url;                     // 原始请求 URL（写入时去控制符 + 截断）
        QString error;                   // 失败原因原文（成功恢复时为空）
        bool blocked = false;            // 是否因反爬拦截耗尽而失败
        int blockAttempts = 0;           // 渲染期间拦截页命中次数（含重试中每次）
        int settleMs = 0;                // 请求的 loadFinished 后 settle 时长
        std::uint8_t outputs = 0;        // 请求的输出类型位标记
        qint64 createdAtMsec = 0;        // 提交时刻
        qint64 startedAtMsec = 0;        // 开始渲染时刻
        qint64 finishedAtMsec = 0;       // 终态时刻
        int queuePending = 0;            // 终态时待渲染任务数（负载快照）
        int queueRunning = 0;            // 终态时渲染中任务数（负载快照）
    };
    void recordRender(const RenderRecord& r);

    // 入口拒绝记录（任务未建立）：stage="ssrf_blocked" / "overloaded"。
    void recordRejected(const QString& url, const QString& stage, const QString& detail);

    // 上下文注入：当前代理摘要（"direct" / "http://h:p" / "socks5://h:p"，不含凭据）。
    // RenderPool 的 GUI 线程代理 apply 定时器周期刷新（与实际生效代理秒级同步）。
    void setProxySummary(QString summary);

    // 累计写入条数（可观测，日志用）。
    std::int64_t writtenTotal() const;

private:
    // 序列化 + 追加一行（持锁调用）。失败静默（ensureOpenLocked 已限频告警）。
    void writeLineLocked(const QByteArray& json);
    // 确保当天文件已打开（按天轮转 + 保留期清理）。失败返回 false 并限频告警。
    bool ensureOpenLocked(const QDate& today);

    mutable QMutex m_mutex;
    QString m_dataDir;                 // 空串 = 禁用
    QFile m_file;                      // 当前打开的日志文件（追加模式）
    QDate m_openDay;                   // 当前文件对应的本地日期（轮转判据）
    QString m_proxySummary = QStringLiteral("direct");  // 最近一次 apply 的代理摘要
    std::int64_t m_written = 0;        // 累计写入行数
    QDate m_lastWarnDay;               // 打开失败告警限频（每天至多一次）

    // 旧日志保留天数（轮转时按文件名日期清理）。
    static constexpr int kRetentionDays = 30;
};

} // namespace seimi
