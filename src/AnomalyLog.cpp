// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "AnomalyLog.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <cstdio>

namespace seimi {

namespace {

// 记录 schema 版本（顶层 "v" 字段）。字段增删时递增并在此注明兼容策略。
constexpr int kSchemaVersion = 1;

// 单字段截断上限（字节）：URL 与错误串可能被异常页面撑得极长，防单行 JSONL 失控。
constexpr int kMaxUrlLen = 4096;
constexpr int kMaxErrLen = 2048;

// 构建版本（CMake configure 时刻注入；#ifndef 兜底兼容项目 CMake 之外的手动编译）。
#ifndef SEIMI_VERSION
#define SEIMI_VERSION "unknown"
#endif
#ifndef SEIMI_GIT_COMMIT
#define SEIMI_GIT_COMMIT "unknown"
#endif

// 去控制符/格式符（防 JSONL 行被换行/控制字节撕裂 + 终端错乱）+ 截断。
QString sanitizeField(const QString& s, int maxLen) {
    QString out;
    out.reserve(qMin(s.size(), maxLen));
    for (const QChar c : s) {
        if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format) {
            continue;
        }
        out.append(c);
    }
    return out.left(maxLen);
}

QString extractHost(const QString& url) {
    const QString host = QUrl(url).host().toLower();
    return host.isEmpty() ? QStringLiteral("(unknown)") : host;
}

// 把错误原文归类为稳定的失败阶段标签（stage），供离线按根因聚合。
// 新增失败路径时在此补映射；归类只做前缀/特征匹配，不影响 error 原文。
QString classifyStage(const QString& error, bool blocked) {
    if (blocked) return QStringLiteral("antibot_blocked");
    if (error.startsWith(QLatin1String("blocked:")))
        return QStringLiteral("antibot_blocked");
    if (error.contains(QLatin1String("Chromium network error page")))
        return QStringLiteral("net_unreachable");
    if (error.startsWith(QLatin1String("page load finished with error")))
        return QStringLiteral("load_error");
    if (error.startsWith(QLatin1String("render timed out")))
        return QStringLiteral("timeout");
    if (error.startsWith(QLatin1String("screenshot:")))
        return QStringLiteral("screenshot_failed");
    return QStringLiteral("other");
}

// OutputMask 位标记 -> 可读名（"html,markdown"）。与 RenderQueue.cpp 的同名辅助保持一致。
QString outputNames(std::uint8_t mask) {
    QStringList parts;
    if (mask & 0x01) parts << QStringLiteral("html");
    if (mask & 0x04) parts << QStringLiteral("markdown");
    if (mask & 0x08) parts << QStringLiteral("pdf");
    if (mask & 0x10) parts << QStringLiteral("screenshot");
    return parts.isEmpty() ? QStringLiteral("(none)") : parts.join(QLatin1Char(','));
}

} // namespace

AnomalyLog::AnomalyLog(const QString& dataDir)
    : m_dataDir(dataDir) {}

AnomalyLog::~AnomalyLog() {
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) m_file.close();
}

void AnomalyLog::setProxySummary(QString summary) {
    QMutexLocker locker(&m_mutex);
    m_proxySummary = std::move(summary);
}

std::int64_t AnomalyLog::writtenTotal() const {
    QMutexLocker locker(&m_mutex);
    return m_written;
}

void AnomalyLog::recordRender(const RenderRecord& r) {
    // 先在栈上组装 JSON（无锁），再进锁写一行；写路径不触碰 RenderQueue 状态。
    const QDateTime now = QDateTime::currentDateTime();

    QJsonObject o;
    o[QStringLiteral("v")] = kSchemaVersion;
    o[QStringLiteral("kind")] = r.succeeded ? QStringLiteral("blocked_recovered")
                                            : QStringLiteral("failed");
    o[QStringLiteral("ts")] = now.toString(Qt::ISODateWithMs);
    o[QStringLiteral("ts_ms")] = now.toMSecsSinceEpoch();
    o[QStringLiteral("task_id")] = r.taskId;
    const QString url = sanitizeField(r.url, kMaxUrlLen);
    o[QStringLiteral("url")] = url;
    o[QStringLiteral("host")] = extractHost(url);
    o[QStringLiteral("stage")] = classifyStage(r.error, r.blocked);
    o[QStringLiteral("error")] = sanitizeField(r.error, kMaxErrLen);
    o[QStringLiteral("blocked")] = r.blocked;
    o[QStringLiteral("block_attempts")] = r.blockAttempts;
    o[QStringLiteral("settle_ms")] = r.settleMs;
    o[QStringLiteral("outputs")] = outputNames(r.outputs);
    // 分段耗时：排队（提交->开始渲染）/ 渲染（开始->终态）/ 端到端。
    const qint64 queueWait = (r.startedAtMsec > r.createdAtMsec)
                                 ? (r.startedAtMsec - r.createdAtMsec) : 0;
    const qint64 renderMs = (r.finishedAtMsec > r.startedAtMsec)
                                ? (r.finishedAtMsec - r.startedAtMsec) : 0;
    const qint64 totalMs = (r.finishedAtMsec > r.createdAtMsec)
                               ? (r.finishedAtMsec - r.createdAtMsec) : 0;
    o[QStringLiteral("queue_wait_ms")] = queueWait;
    o[QStringLiteral("render_ms")] = renderMs;
    o[QStringLiteral("total_ms")] = totalMs;
    o[QStringLiteral("queue_pending")] = r.queuePending;
    o[QStringLiteral("queue_running")] = r.queueRunning;
    o[QStringLiteral("version")] = QStringLiteral(SEIMI_VERSION);
    o[QStringLiteral("commit")] = QStringLiteral(SEIMI_GIT_COMMIT);

    // 组装在锁外完成，进锁后只读代理摘要 + 写一行（最短持锁时间）。
    QMutexLocker locker(&m_mutex);
    o[QStringLiteral("proxy")] = m_proxySummary;
    writeLineLocked(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void AnomalyLog::recordRejected(const QString& url, const QString& stage, const QString& detail) {
    const QDateTime now = QDateTime::currentDateTime();

    QJsonObject o;
    o[QStringLiteral("v")] = kSchemaVersion;
    o[QStringLiteral("kind")] = QStringLiteral("rejected");
    o[QStringLiteral("ts")] = now.toString(Qt::ISODateWithMs);
    o[QStringLiteral("ts_ms")] = now.toMSecsSinceEpoch();
    const QString cleanUrl = sanitizeField(url, kMaxUrlLen);
    o[QStringLiteral("url")] = cleanUrl;
    o[QStringLiteral("host")] = extractHost(cleanUrl);
    o[QStringLiteral("stage")] = stage;
    o[QStringLiteral("detail")] = sanitizeField(detail, kMaxErrLen);
    o[QStringLiteral("version")] = QStringLiteral(SEIMI_VERSION);
    o[QStringLiteral("commit")] = QStringLiteral(SEIMI_GIT_COMMIT);

    QMutexLocker locker(&m_mutex);
    o[QStringLiteral("proxy")] = m_proxySummary;
    writeLineLocked(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool AnomalyLog::ensureOpenLocked(const QDate& today) {
    if (m_dataDir.isEmpty()) return false;
    if (m_file.isOpen() && m_openDay == today) return true;

    if (m_file.isOpen()) m_file.close();

    // 轮转时机顺带清理超保留期的旧文件（每天一次，O(目录条目数)，代价可忽略）。
    QDir dir(m_dataDir + QStringLiteral("/anomalies"));
    const QString prefix = QStringLiteral("anomaly-");
    const QDate expireBefore = today.addDays(-kRetentionDays);
    const QStringList olds = dir.entryList({QStringLiteral("anomaly-????????.jsonl")},
                                           QDir::Files, QDir::Name);
    for (const QString& name : olds) {
        const QDate d = QDate::fromString(name.mid(prefix.size(), 8), QStringLiteral("yyyyMMdd"));
        if (d.isValid() && d < expireBefore) QFile::remove(dir.filePath(name));
    }

    if (!dir.mkpath(QStringLiteral("."))) {
        if (m_lastWarnDay != today) {
            std::fprintf(stderr, "[anomaly] WARN: cannot create %s, anomaly logging disabled\n",
                         dir.path().toUtf8().constData());
            m_lastWarnDay = today;
        }
        return false;
    }

    const QString path = dir.filePath(
        QStringLiteral("anomaly-%1.jsonl").arg(today.toString(QStringLiteral("yyyyMMdd"))));
    const bool created = !QFile::exists(path);
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        if (m_lastWarnDay != today) {
            std::fprintf(stderr, "[anomaly] WARN: cannot open %s for append (%s)\n",
                         path.toUtf8().constData(),
                         m_file.errorString().toUtf8().constData());
            m_lastWarnDay = today;
        }
        return false;
    }
    if (created) {
        // URL query 可能含凭据：仅属主可读写。
        m_file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }
    m_openDay = today;
    return true;
}

void AnomalyLog::writeLineLocked(const QByteArray& json) {
    if (!ensureOpenLocked(QDate::currentDate())) return;

    m_file.write(json);
    m_file.write("\n", 1);
    m_file.flush();   // 异常数据珍贵，逐条落盘
    ++m_written;
}

} // namespace seimi
