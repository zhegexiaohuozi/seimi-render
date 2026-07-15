// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Metrics.h"
#include "RenderTask.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSharedPointer>
#include <QString>
#include <QWaitCondition>

#include <deque>
#include <vector>

namespace seimi {

using RenderTaskPtr = QSharedPointer<RenderTask>;

// 全局线程安全中枢：连接 HTTP/WS 线程（生产者）与 GUI 线程渲染池（消费者），
// 并承载任务结果供长轮询/WebSocket 取用。
// 所有公有方法可被任意线程调用，内部用单一 QMutex 串行化。
// GUI 渲染线程用 tryTakePending() 非阻塞轮询（事件循环回调里绝不阻塞）。
// HTTP 长轮询线程用 waitForCompletion() 阻塞等待终态。终态后的 WS 推送由调用方在主线程信号槽完成。
class RenderQueue {
public:
    RenderQueue() = default;
    ~RenderQueue() = default;

    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    // 生产者：HTTP/WS 线程提交任务，返回 task id。
    // 任务表达 kMaxLiveTasks（过载）时返回空串——调用方据此回 503 做背压。
    QString submit(QString url, int settleDelayMsec, OutputMask outputs,
                   ImageFormat imageFormat, MdAlgorithm mdAlgorithm, qint64 nowMsec,
                   ExtractAlgorithm extractAlgorithm = ExtractAlgorithm::None);

    // 消费者：GUI 渲染线程【非阻塞】取一个 Pending 任务并置 Running。
    // 队列空时立即返回 nullptr——供事件循环回调轮询，绝不阻塞。
    RenderTaskPtr tryTakePending();

    // 渲染成功结果的载体：按请求的 output 填充对应字段。
    struct RenderResult {
        QString html;        // 始终填充（markdown 依赖）
        QString markdown;    // 请求 markdown 时填充
        QByteArray pdfData;  // 请求 pdf 时填充
        QByteArray imageData;// 请求 screenshot 时填充（PNG 或 JPEG）
        ImageFormat imageFmt{ImageFormat::Png};  // 截图实际编码格式（供 /image 路由设 Content-Type）
        bool imageTruncated{false};              // 截图是否因超高度上限被截断
        MdAlgorithm mdAlgorithmUsed{MdAlgorithm::Conservative};  // markdown 实际算法（可观测性）
        QString serpJson;                   // SERP 结构化提取结果 JSON（extract!=None 且成功时填）
    };

    // 任务的可观测快照（持锁拷贝）。供 HTTP/WS 工作线程安全读取任务字段——
    // 直接持有 RenderTaskPtr 跨线程读可变字段是数据竞争，快照在锁内拷出后与写者无关。
    struct Snapshot {
        bool found = false;                // 任务是否存在（false=未找到/已淘汰）
        bool done = false;                 // 终态标志
        TaskState state{TaskState::Pending};
        QString id;
        QString url;
        QString error;                     // 失败原因（done 且 Failed 时有意义）
        QString html;                      // 渲染后完整 HTML（成功时）
        QString markdown;                  // HTML 转 markdown（请求了 markdown 时）
        QByteArray pdfData;                // PDF 字节（请求了 pdf 时）
        QByteArray imageData;              // 截图字节（请求了 screenshot 时）
        ImageFormat resolvedImageFmt{ImageFormat::Png};  // 截图实际编码格式
        bool imageTruncated = false;       // 截图是否因超高度上限被截断
        MdAlgorithm mdAlgorithmUsed{MdAlgorithm::Conservative};  // markdown 实际算法
        QString serpJson;                   // SERP 结构化提取结果 JSON（成功时）
        OutputMask outputs = 0;            // 请求的输出类型位标记
        qint64 createdAtMsec = 0;
        qint64 startedAtMsec = 0;
        qint64 finishedAtMsec = 0;
    };

    // GUI 渲染线程：把任务标记为成功并写入结果，唤醒所有等待该任务的长轮询线程。
    void reportSucceeded(const QString& id, const RenderResult& result, qint64 nowMsec);

    // GUI 渲染线程：把任务标记为失败，唤醒所有等待该任务的长轮询线程。
    void reportFailed(const QString& id, QString error, qint64 nowMsec);

    // 长轮询：阻塞至多 timeoutMsec，直到该任务到达终态。
    // 超时则返回当前指针（可能仍非终态，由调用方判 done）。仅供同线程（GUI）使用，跨线程读用 snapshot()。
    RenderTaskPtr waitForCompletion(const QString& id, unsigned long timeoutMsec);

    // 非阻塞查询（GET /status, GET /result）。
    RenderTaskPtr peek(const QString& id) const;

    // 非阻塞安全快照：锁内拷贝任务可观测字段，避免跨线程直接访问 RenderTask 可变字段（数据竞争）。
    Snapshot snapshot(const QString& id) const;

    // 停止队列：唤醒所有消费者与长轮询线程，令它们退出。
    void stop();

    // 由 RenderPool 在 GUI 线程 start() 后调一次：登记 worker 并发槽位总数（供 stats/UI）。
    // 与 worker 状态不同：槽位数是静态配置（--concurrency），不随负载变化；故一次性设置即可。
    void setConcurrency(int n);

    // 统计信息（供 /stats 与 /status 的 queue 字段）。
    struct Stats {
        int total{0};       // 任务表大小
        int pending{0};     // 待渲染（当前队列堆积）
        int running{0};     // 渲染中
        int done{0};        // 终态
        int evicted{0};     // 累计已淘汰的终态任务数（内存边界可观测）
        int concurrency{0}; // worker 并发槽位总数（静态，由 setConcurrency 登记）
        int busy{0};        // 正在渲染的 worker 数（语义别名 = running，便于 UI 表达「活跃 worker」）
        int peakPending{0}; // pending 队列历史最大堆积长度（单调不降，反映历史峰值负载）
    };
    Stats stats() const;

    // 累计运行时指标（供 GET /status）。Metrics 自身线程安全（内部带锁），
    // 故可直接返回引用，查询方调用 snapshot() 时自行加锁。
    Metrics& metrics() { return m_metrics; }
    const Metrics& metrics() const { return m_metrics; }

private:
    // 设置终态的通用实现（成功/失败共用）。
    void setTerminal(const QString& id,
                     TaskState state,
                     const std::function<void(RenderTask&)>& fill,
                     qint64 nowMsec);

    // 淘汰过期/超量的终态任务（内存边界，防产物无限堆积致内存耗尽 DoS）。
    // 调用方必须已持有 m_mutex。
    void sweepTerminalLocked(qint64 nowMsec);

    // 任务表内存边界（防产物无限堆积致 OOM）：
    //   kMaxLiveTasks —— 表硬上限，submit 超限返回空 id（503 背压）。
    //   kMaxTerminalTasks —— 终态任务保留上限，超限按完成时间淘汰最旧者。
    //   kTaskRetentionMsec —— 终态任务保留时长，超时淘汰。
    static constexpr int kMaxLiveTasks = 1000;
    static constexpr int kMaxTerminalTasks = 300;
    static constexpr qint64 kTaskRetentionMsec = 5 * 60 * 1000;

    mutable QMutex m_mutex;
    QWaitCondition m_doneCond;       // 长轮询等待：任一任务到达终态

    std::deque<QString> m_pendingIds;                 // FIFO 待渲染队列
    QHash<QString, RenderTaskPtr> m_tasks;            // 任务表（id -> task）
    int m_evicted = 0;                                // 累计已淘汰终态任务数（可观测）
    int m_concurrency = 0;                            // worker 并发槽位总数（setConcurrency 登记）
    int m_peakPending = 0;                            // pending 队列历史峰值（submit 时更新）
    std::atomic<bool> m_stopped{false};

    Metrics m_metrics;                                // 累计运行时指标
};

} // namespace seimi
