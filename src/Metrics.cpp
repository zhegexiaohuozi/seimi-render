// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "Metrics.h"
#include "RenderTask.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace seimi {

namespace {
// 当前 epoch 毫秒。
inline std::int64_t epochMsec() {
    return std::int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 持久化文件名 + schema 版本。version 写进 JSON 顶层，升级时改 parse 兼容逻辑。
constexpr const char* kMetricsFile = "metrics.json";
constexpr std::int64_t kSchemaVersion = 1;
} // namespace

Metrics::Metrics(const QString& dataDir)
    : m_dataDir(dataDir), m_startedAtMsec(epochMsec()) {
    loadFromDisk();
}

void Metrics::addToHistogram(std::int64_t ms) {
    // ms -> 桶下标：floor(log2(ms))，clamp 到 [0, kBucketCount-1]。
    // 每个桶覆盖一倍区间（桶 i = [2^i, 2^(i+1))），O(1) 更新，固定内存。
    int idx = int(std::log2(double(ms < 1 ? 1 : ms)));
    if (idx < 0) idx = 0;
    if (idx >= kBucketCount) idx = kBucketCount - 1;
    m_latencyBuckets[idx]++;
    m_latencySumMs += ms;
    if (m_latencyMinMs == 0 || ms < m_latencyMinMs) m_latencyMinMs = ms;
    if (ms > m_latencyMaxMs) m_latencyMaxMs = ms;
}

void Metrics::record(std::string host, bool succeeded, std::int64_t elapsedMs, std::uint8_t outputs,
                     int blockAttempts, bool blockedFinal) {
    QMutexLocker locker(&m_mutex);

    ++m_total;

    // 反爬拦截计数：事件总数（含重试中每次命中）/ 恢复 / 耗尽三态分开记。
    m_blockedTotal += blockAttempts;
    if (succeeded && blockAttempts > 0) ++m_blockedRecovered;
    if (blockedFinal) ++m_blockedExhausted;

    if (succeeded) {
        ++m_succeeded;
        if (elapsedMs > 0) addToHistogram(elapsedMs);
    }

    // 输出类型需求统计（位标记）
    if (outputs & static_cast<std::uint8_t>(Output::Html))       ++m_outputHtml;
    if (outputs & static_cast<std::uint8_t>(Output::Markdown))   ++m_outputMarkdown;
    if (outputs & static_cast<std::uint8_t>(Output::Pdf))        ++m_outputPdf;
    if (outputs & static_cast<std::uint8_t>(Output::Screenshot)) ++m_outputScreenshot;

    // 域名分布。空 host（异常 URL）归为 "(unknown)"。
    if (host.empty()) host = "(unknown)";
    auto it = m_domains.find(host);
    if (it == m_domains.end()) {
        // 域名映射达到上限：剔除当前计数最少的冷门域，再插入新域。
        // 均摊成本低（仅在新增域时触发，且通常远不达上限）。
        if (static_cast<int>(m_domains.size()) >= kMaxHosts) {
            auto victim = m_domains.begin();
            for (auto itv = m_domains.begin(); itv != m_domains.end(); ++itv) {
                if (itv->second.total < victim->second.total) victim = itv;
            }
            m_domains.erase(victim);
        }
        it = m_domains.emplace(host, HostCount{}).first;
    }
    ++it->second.total;
    if (succeeded) ++it->second.succeeded; else ++it->second.failed;
    it->second.blocked += blockAttempts;
}

Metrics::Snapshot Metrics::snapshot(int maxDomains) const {
    QMutexLocker locker(&m_mutex);
    const std::int64_t now = epochMsec();

    Snapshot s;
    s.startedAtMsec = m_startedAtMsec;
    s.uptimeMsec = (m_startedAtMsec == 0) ? 0 : (now - m_startedAtMsec);
    s.total = m_total;
    s.succeeded = m_succeeded;
    s.failed = m_total - m_succeeded;
    s.successRate = (m_total == 0) ? 0.0 : double(m_succeeded) / double(m_total);
    s.blockedTotal = m_blockedTotal;
    s.blockedRecovered = m_blockedRecovered;
    s.blockedExhausted = m_blockedExhausted;

    // 延迟分位数：按桶累计，找到对应百分位落入的桶，桶内用线性插值。
    std::int64_t succCount = 0;
    for (auto c : m_latencyBuckets) succCount += c;
    if (succCount > 0) {
        s.latencyMinMs = m_latencyMinMs;
        s.latencyMaxMs = m_latencyMaxMs;
        s.latencyAvgMs = double(m_latencySumMs) / double(succCount);
        auto pct = [&](double q) -> std::int64_t {
            if (succCount <= 0) return 0;            // 无样本：分位数为 0
            double target = q * double(succCount);   // 目标排名（1-based 区间内）
            if (target < 1.0) target = 1.0;          // 至少第 1 个样本
            std::int64_t acc = 0;
            for (int i = 0; i < kBucketCount; ++i) {
                acc += m_latencyBuckets[i];
                if (double(acc) >= target) {
                    // 该任务落入第 i 桶：区间 [2^i, 2^(i+1))。
                    std::int64_t lo = (i == 0) ? 1 : (std::int64_t(1) << i);
                    std::int64_t hi = (i + 1 >= kBucketCount) ? m_latencyMaxMs
                                                              : (std::int64_t(1) << (i + 1));
                    if (hi <= lo) hi = lo + 1;
                    // 桶内线性插值到桶中点（够用且无需逐样本存储）。
                    return (lo + hi) / 2;
                }
            }
            return m_latencyMaxMs;
        };
        s.latencyP50Ms = pct(0.50);
        s.latencyP90Ms = pct(0.90);
        s.latencyP99Ms = pct(0.99);
    }

    // 吞吐
    if (s.uptimeMsec > 0) {
        s.throughputPerSec = double(m_total) / (double(s.uptimeMsec) / 1000.0);
    }

    s.outputHtml = m_outputHtml;
    s.outputMarkdown = m_outputMarkdown;
    s.outputPdf = m_outputPdf;
    s.outputScreenshot = m_outputScreenshot;

    // 域名分布：拷贝后排序取 top-N。
    s.distinctDomains = std::int64_t(m_domains.size());
    std::vector<DomainStat> all;
    all.reserve(m_domains.size());
    for (const auto& kv : m_domains) {
        all.push_back({kv.first, kv.second.total, kv.second.succeeded, kv.second.failed, kv.second.blocked});
    }
    std::sort(all.begin(), all.end(), [](const DomainStat& a, const DomainStat& b) {
        return a.total > b.total;   // 按 total 倒序
    });
    if (maxDomains > 0 && int(all.size()) > maxDomains) {
        all.resize(maxDomains);
    }
    s.domains = std::move(all);

    return s;
}

// ====== 持久化 ======

void Metrics::loadFromDisk() {
    // 构造期调用，无并发。dataDir 为空 = 禁用。
    if (m_dataDir.isEmpty()) return;

    QString path = m_dataDir + "/" + kMetricsFile;
    QFile f(path);
    if (!f.exists()) return;                        // 首次启动：正常，无日志
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "[metrics] WARN: failed to open %s for read, starting fresh\n",
                     path.toUtf8().constData());
        return;
    }
    QByteArray data = f.readAll();
    f.close();

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::fprintf(stderr, "[metrics] WARN: %s is corrupted (%s), starting fresh\n",
                     path.toUtf8().constData(), err.errorString().toUtf8().constData());
        return;
    }
    QJsonObject o = doc.object();
    std::int64_t version = o.value("version").toVariant().toLongLong();
    if (!parse(data, version)) {
        std::fprintf(stderr, "[metrics] WARN: %s schema (v%lld) parse failed, starting fresh\n",
                     path.toUtf8().constData(), static_cast<long long>(version));
        // parse 失败时字段可能已被部分写入——重置为安全基线。
        m_total = m_succeeded = m_blockedTotal = m_blockedRecovered = m_blockedExhausted = 0;
        m_latencyBuckets.assign(kBucketCount, 0);
        m_latencySumMs = m_latencyMinMs = m_latencyMaxMs = 0;
        m_outputHtml = m_outputMarkdown = m_outputPdf = m_outputScreenshot = 0;
        m_domains.clear();
    } else {
        std::fprintf(stdout, "[metrics] restored cumulative stats from %s "
                             "(total=%lld, succeeded=%lld, domains=%zu)\n",
                     path.toUtf8().constData(),
                     static_cast<long long>(m_total), static_cast<long long>(m_succeeded),
                     m_domains.size());
    }
}

void Metrics::saveToDisk() {
    // GUI 线程调用（QTimer / aboutToQuit 回调）。dataDir 为空 = 禁用。
    if (m_dataDir.isEmpty()) return;

    // 锁内拷贝快照（快），释锁后做序列化 + IO（慢，不阻塞 record 热路径）。
    QByteArray json;
    {
        QMutexLocker locker(&m_mutex);
        json = serializeLocked();
    }
    if (json.isEmpty()) return;

    // 原子写：先写 .tmp，再 rename 覆盖（防写一半崩溃损坏）。
    QDir().mkpath(m_dataDir);
    QString tmpPath = m_dataDir + "/" + kMetricsFile + ".tmp";
    QString finalPath = m_dataDir + "/" + kMetricsFile;

    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "[metrics] WARN: cannot write %s\n", tmpPath.toUtf8().constData());
        return;
    }
    tmp.write(json);
    tmp.flush();
    tmp.close();
    tmp.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    // rename 覆盖：POSIX 原子。Windows 下 QFile::rename 会因目标存在失败，先删目标。
    if (QFile::exists(finalPath)) QFile::remove(finalPath);
    if (!tmp.rename(finalPath)) {
        std::fprintf(stderr, "[metrics] WARN: rename %s -> %s failed\n",
                     tmpPath.toUtf8().constData(), finalPath.toUtf8().constData());
        QFile::remove(tmpPath);   // 清理残留 tmp
    }
}

QByteArray Metrics::serializeLocked() const {
    // 调用方持 m_mutex。只序列化累计字段；uptime/startedAt/派生量不存。
    QJsonObject o;
    o["version"] = qint64(kSchemaVersion);
    o["total"] = qint64(m_total);
    o["succeeded"] = qint64(m_succeeded);
    o["blockedTotal"] = qint64(m_blockedTotal);
    o["blockedRecovered"] = qint64(m_blockedRecovered);
    o["blockedExhausted"] = qint64(m_blockedExhausted);
    o["latencySumMs"] = qint64(m_latencySumMs);
    o["latencyMinMs"] = qint64(m_latencyMinMs);
    o["latencyMaxMs"] = qint64(m_latencyMaxMs);
    o["outputHtml"] = qint64(m_outputHtml);
    o["outputMarkdown"] = qint64(m_outputMarkdown);
    o["outputPdf"] = qint64(m_outputPdf);
    o["outputScreenshot"] = qint64(m_outputScreenshot);

    QJsonArray buckets;
    for (auto c : m_latencyBuckets) buckets.append(qint64(c));
    o["latencyBuckets"] = buckets;

    QJsonArray doms;
    for (const auto& kv : m_domains) {
        QJsonObject d;
        d["host"] = QString::fromStdString(kv.first);
        d["total"] = qint64(kv.second.total);
        d["succeeded"] = qint64(kv.second.succeeded);
        d["failed"] = qint64(kv.second.failed);
        d["blocked"] = qint64(kv.second.blocked);
        doms.append(d);
    }
    o["domains"] = doms;

    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool Metrics::parse(const QByteArray& json, std::int64_t schemaVersion) {
    if (schemaVersion != kSchemaVersion) return false;   // 仅认 v1，未来版本单独兼容

    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return false;
    QJsonObject o = doc.object();

    auto getI64 = [&](const char* key) -> std::int64_t {
        return o.value(key).toVariant().toLongLong();
    };

    m_total = getI64("total");
    m_succeeded = getI64("succeeded");
    // failed 是派生量（total - succeeded），不存。
    m_blockedTotal = getI64("blockedTotal");
    m_blockedRecovered = getI64("blockedRecovered");
    m_blockedExhausted = getI64("blockedExhausted");
    m_latencySumMs = getI64("latencySumMs");
    m_latencyMinMs = getI64("latencyMinMs");
    m_latencyMaxMs = getI64("latencyMaxMs");
    m_outputHtml = getI64("outputHtml");
    m_outputMarkdown = getI64("outputMarkdown");
    m_outputPdf = getI64("outputPdf");
    m_outputScreenshot = getI64("outputScreenshot");

    // 延迟桶：32 个。长度不符 = 损坏。
    QJsonArray buckets = o.value("latencyBuckets").toArray();
    if (buckets.size() != kBucketCount) return false;
    for (int i = 0; i < kBucketCount; ++i) {
        m_latencyBuckets[i] = buckets.at(i).toVariant().toLongLong();
    }

    // 域名分布：load 时按 total 倒序填入，超 kMaxHosts 截断（与运行时淘汰一致）。
    m_domains.clear();
    QJsonArray doms = o.value("domains").toArray();
    struct D { std::string host; std::int64_t total; HostCount c; };
    std::vector<D> sorted;
    sorted.reserve(doms.size());
    for (const auto& v : doms) {
        QJsonObject d = v.toObject();
        D e;
        e.host = d.value("host").toString().toStdString();
        e.c.total = d.value("total").toVariant().toLongLong();
        e.c.succeeded = d.value("succeeded").toVariant().toLongLong();
        e.c.failed = d.value("failed").toVariant().toLongLong();
        e.c.blocked = d.value("blocked").toVariant().toLongLong();
        e.total = e.c.total;
        if (!e.host.empty()) sorted.push_back(std::move(e));
    }
    std::sort(sorted.begin(), sorted.end(), [](const D& a, const D& b) { return a.total > b.total; });
    for (auto& e : sorted) {
        if (static_cast<int>(m_domains.size()) >= kMaxHosts) break;
        m_domains.emplace(std::move(e.host), e.c);
    }
    return true;
}

} // namespace seimi
