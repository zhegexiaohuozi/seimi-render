#include "Metrics.h"
#include "RenderTask.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace seimi {

namespace {
// 当前 epoch 毫秒。
inline std::int64_t epochMsec() {
    return std::int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
} // namespace

Metrics::Metrics() : m_startedAtMsec(epochMsec()) {}

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

void Metrics::record(std::string host, bool succeeded, std::int64_t elapsedMs, std::uint8_t outputs) {
    QMutexLocker locker(&m_mutex);

    ++m_total;
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
        all.push_back({kv.first, kv.second.total, kv.second.succeeded, kv.second.failed});
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

} // namespace seimi
