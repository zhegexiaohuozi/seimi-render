// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QMutex>
#include <QMutexLocker>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace seimi {

// 运行时指标聚合。低开销、内存有界。
// 只在任务到达终态时调 record()（RenderQueue 持锁 setTerminal 内部触发），
// 故本类用独立 QMutex，不与渲染队列锁嵌套。
// 延迟用对数桶直方图（固定 32 桶，O(1)）；域名计数设上限（默认 1000），超限剔除最冷门域。
class Metrics {
public:
    Metrics();

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // 记录一个到达终态的任务。
    //   host      : 提取自 URL 的注册域名（或 host[:port]）
    //   succeeded : 是否成功
    //   elapsedMs : 从开始渲染到完成的耗时（含排队前的渲染耗时；不含排队等待）
    //   outputs   : 请求的输出类型位标记（用于统计 html/markdown/pdf 需求分布）
    void record(std::string host, bool succeeded, std::int64_t elapsedMs, std::uint8_t outputs);

    // ====== 运行时快照（GET /status 用）======

    struct DomainStat {
        std::string host;
        std::int64_t total = 0;
        std::int64_t succeeded = 0;
        std::int64_t failed = 0;
    };

    struct Snapshot {
        // 进程信息
        std::int64_t startedAtMsec = 0;     // 启动时刻
        std::int64_t uptimeMsec = 0;        // 运行时长
        // 累计计数（自启动起）
        std::int64_t total = 0;
        std::int64_t succeeded = 0;
        std::int64_t failed = 0;
        double successRate = 0.0;           // 0..1
        // 延迟分布（仅成功任务的渲染耗时）
        std::int64_t latencyMinMs = 0;
        std::int64_t latencyMaxMs = 0;
        double latencyAvgMs = 0.0;
        std::int64_t latencyP50Ms = 0;
        std::int64_t latencyP90Ms = 0;
        std::int64_t latencyP99Ms = 0;
        // 吞吐
        double throughputPerSec = 0.0;      // 终态任务 / 运行时长(秒)
        // 输出类型需求分布（请求次数）
        std::int64_t outputHtml = 0;
        std::int64_t outputMarkdown = 0;
        std::int64_t outputPdf = 0;
        std::int64_t outputScreenshot = 0;
        // 域名分布（按 total 倒序，取前 maxDomains 个）
        std::vector<DomainStat> domains;
        std::int64_t distinctDomains = 0;
    };

    // 取一份快照。maxDomains 限制返回的域名条数（默认 20）。
    Snapshot snapshot(int maxDomains = 20) const;

private:
    // 把一个成功任务的延迟计入直方图桶。
    void addToHistogram(std::int64_t ms);

    mutable QMutex m_mutex;
    std::int64_t m_startedAtMsec = 0;       // 构造（进程启动）时初始化

    // 累计计数
    std::int64_t m_total = 0;
    std::int64_t m_succeeded = 0;

    // 成功延迟：直方图桶 + 总和（用于算 avg）
    // 桶按 2 的幂划分（powers-of-two style），ms -> bucket index = floor(log2(ms))+1
    // 落桶后用线性插值算分位数。固定大小，O(1) 更新。
    static constexpr int kBucketCount = 32; // 覆盖 1ms ~ 2^31 ms
    // 注意：用 () 而非 {} 初始化——vector<int>{n,0} 会被当成 initializer_list
    // 构造出 [n,0] 两个元素，而非 n 个 0。这里要的是 count+value 构造。
    std::vector<std::int64_t> m_latencyBuckets = std::vector<std::int64_t>(kBucketCount, 0);
    std::int64_t m_latencySumMs = 0;
    std::int64_t m_latencyMinMs = 0;
    std::int64_t m_latencyMaxMs = 0;

    // 输出类型需求计数
    std::int64_t m_outputHtml = 0;
    std::int64_t m_outputMarkdown = 0;
    std::int64_t m_outputPdf = 0;
    std::int64_t m_outputScreenshot = 0;

    // 域名分布：host -> {total, succeeded, failed}
    struct HostCount {
        std::int64_t total = 0;
        std::int64_t succeeded = 0;
        std::int64_t failed = 0;
    };
    std::unordered_map<std::string, HostCount> m_domains;
    static constexpr int kMaxHosts = 1000;  // 域名映射上限，超限剔除最冷门者
};

} // namespace seimi
