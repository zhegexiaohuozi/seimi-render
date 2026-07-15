#include "RenderQueue.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace seimi {

// 生成短任务 id：UUID 去掉连字符，前 16 字符。避免全局碰撞又足够紧凑。
static QString makeTaskId() {
    QString u = QUuid::createUuid().toString(QUuid::WithoutBraces); // 36 chars
    u.remove('-');
    return u.left(16);
}

// 从 URL 抽取 host（去端口），用于域名分布统计。
// 不做 Public Suffix List 级别的注册域提取（会引入额外依赖/体积），
// 直接用 host 粒度，足以反映流量来源分布。
static std::string extractHost(const QString& url) {
    const QUrl u(url);
    QString host = u.host().toLower();
    return host.isEmpty() ? std::string() : host.toStdString();
}

QString RenderQueue::submit(QString url, int settleDelayMsec, OutputMask outputs,
                            ImageFormat imageFormat, MdAlgorithm mdAlgorithm, qint64 nowMsec,
                            ExtractAlgorithm extractAlgorithm) {
    QMutexLocker locker(&m_mutex);

    // 过载背压：任务表已满（洪泛提交/积压未消化），拒绝新提交。调用方据此回 503。
    if (static_cast<int>(m_tasks.size()) >= kMaxLiveTasks) {
        return {};
    }

    // 防御性 id 去重（碰撞概率极低）。
    QString id = makeTaskId();
    while (m_tasks.contains(id)) id = makeTaskId();

    auto task = RenderTaskPtr::create(id, std::move(url), nowMsec, settleDelayMsec,
                                      outputs, imageFormat, mdAlgorithm, extractAlgorithm);
    m_tasks.insert(id, task);
    m_pendingIds.push_back(id);

    // 更新 pending 队列历史峰值（单调不降，反映曾达到的最大堆积负载）。
    if (static_cast<int>(m_pendingIds.size()) > m_peakPending) {
        m_peakPending = static_cast<int>(m_pendingIds.size());
    }

    // 借机淘汰过期/超量的终态任务，防完成任务的产物（html/pdf/image 可达数十 MB）无限堆积致 OOM。
    sweepTerminalLocked(nowMsec);

    return id;
}

// 淘汰过期/超量的终态任务（调用方持锁）。两道闸门：TTL + 数量上限。O(N)，N 受 kMaxLiveTasks 封顶。
void RenderQueue::sweepTerminalLocked(qint64 nowMsec) {
    // 1) TTL：保留期已过的终态任务直接淘汰。
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        const RenderTaskPtr& t = it.value();
        if (t->done && (nowMsec - t->finishedAtMsec) > kTaskRetentionMsec) {
            it = m_tasks.erase(it);
            ++m_evicted;
        } else {
            ++it;
        }
    }
    // 2) 终态任务数量上限：仍超限时按完成时间升序淘汰最旧者。
    int terminal = 0;
    for (const auto& t : m_tasks) if (t->done) ++terminal;
    if (terminal > kMaxTerminalTasks) {
        std::vector<std::pair<qint64, QString>> byAge;
        byAge.reserve(terminal);
        for (const auto& t : m_tasks) {
            if (t->done) byAge.emplace_back(t->finishedAtMsec, t->id);
        }
        std::sort(byAge.begin(), byAge.end());
        const std::size_t toEvict = static_cast<std::size_t>(terminal - kMaxTerminalTasks);
        for (std::size_t i = 0; i < toEvict && i < byAge.size(); ++i) {
            m_tasks.remove(byAge[i].second);
            ++m_evicted;
        }
    }
}

RenderTaskPtr RenderQueue::tryTakePending() {
    QMutexLocker locker(&m_mutex);
    if (m_pendingIds.empty()) return nullptr;

    QString id = m_pendingIds.front();
    m_pendingIds.pop_front();

    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) return nullptr;

    RenderTaskPtr task = it.value();
    if (task->state != TaskState::Pending) return nullptr;

    task->state = TaskState::Running;
    return task;
}

void RenderQueue::setTerminal(const QString& id,
                              TaskState state,
                              const std::function<void(RenderTask&)>& fill,
                              qint64 nowMsec) {
    // 锁内做终态变更 + 取记录所需字段，释锁后记 Metrics（Metrics 自带锁，避免与渲染队列锁嵌套）。
    std::string host;
    bool succeeded = false;
    qint64 elapsedMs = 0;
    OutputMask outputs = 0;
    bool shouldRecord = false;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_tasks.find(id);
        if (it == m_tasks.end()) return;
        RenderTaskPtr task = it.value();
        if (task->done) return; // 幂等：终态不可逆

        task->state = state;
        task->finishedAtMsec = nowMsec;
        task->done = true;
        if (fill) fill(*task);

        // 准备 Metrics 记录所需字段。
        host = extractHost(task->url);
        succeeded = (state == TaskState::Succeeded);
        // 渲染耗时：从实际开始渲染到完成（startedAtMsec 由 worker 在 assign 时填）。
        const qint64 start = task->startedAtMsec ? task->startedAtMsec : task->createdAtMsec;
        elapsedMs = nowMsec > start ? (nowMsec - start) : 0;
        outputs = task->outputs;
        shouldRecord = true;

        // 借终态变更淘汰过期/超量旧终态任务（刚完成的任务不会被淘汰，WS/长轮询回取安全）。
        sweepTerminalLocked(nowMsec);

        // 唤醒所有长轮询线程（它们各自在 peek 后判断是否是自己等的那一个）。
        m_doneCond.wakeAll();
    }

    if (shouldRecord) {
        m_metrics.record(std::move(host), succeeded, elapsedMs, outputs);
    }
}

void RenderQueue::reportSucceeded(const QString& id, const RenderResult& result, qint64 nowMsec) {
    setTerminal(id, TaskState::Succeeded,
                [&result](RenderTask& t) {
                    t.html = result.html;
                    t.markdown = result.markdown;
                    t.pdfData = result.pdfData;
                    t.imageData = result.imageData;
                    t.resolvedImageFmt = result.imageFmt;
                    t.imageTruncated = result.imageTruncated;
                    t.mdAlgorithmUsed = result.mdAlgorithmUsed;
                    t.serpJson = result.serpJson;
                },
                nowMsec);
}

void RenderQueue::reportFailed(const QString& id, QString error, qint64 nowMsec) {
    setTerminal(id, TaskState::Failed,
                [&error](RenderTask& t) { t.error = std::move(error); },
                nowMsec);
}

RenderTaskPtr RenderQueue::waitForCompletion(const QString& id, unsigned long timeoutMsec) {
    QMutexLocker locker(&m_mutex);
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) return nullptr;
    RenderTaskPtr task = it.value();
    if (task->done) return task;

    // 循环等待：m_doneCond 由任一任务终态 wakeAll，可能被「别的任务完成」误唤醒。
    // 被唤醒后若本任务仍未终态，须在剩余时间内继续等待，否则高并发下长轮询失效。
    QElapsedTimer timer;
    timer.start();
    forever {
        // 剩余等待时间（不少于 0）
        qint64 remaining = qint64(timeoutMsec) - timer.elapsed();
        if (remaining <= 0) break; // 已超时
        m_doneCond.wait(&m_mutex, ulong(remaining));
        // 重新查表（任务可能在唤醒间被清理；理论上不会，但防御性处理）
        it = m_tasks.find(id);
        if (it == m_tasks.end()) return nullptr;
        if (it.value()->done) return it.value();
        // 未完成且未超时 -> 继续循环等待
    }
    return task; // 超时，返回当前（可能仍未完成）的任务
}

RenderTaskPtr RenderQueue::peek(const QString& id) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_tasks.find(id);
    return it == m_tasks.end() ? nullptr : it.value();
}

RenderQueue::Snapshot RenderQueue::snapshot(const QString& id) const {
    QMutexLocker locker(&m_mutex);
    Snapshot s;
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) return s;  // found=false
    const RenderTask& t = *it.value();
    s.found = true;
    s.done = t.done;
    s.state = t.state;
    s.id = t.id;            // const 字段，但拷贝无妨
    s.url = t.url;
    s.error = t.error;
    s.html = t.html;
    s.markdown = t.markdown;
    s.pdfData = t.pdfData;
    s.imageData = t.imageData;
    s.resolvedImageFmt = t.resolvedImageFmt;
    s.imageTruncated = t.imageTruncated;
    s.mdAlgorithmUsed = t.mdAlgorithmUsed;
    s.serpJson = t.serpJson;
    s.outputs = t.outputs;
    s.createdAtMsec = t.createdAtMsec;
    s.startedAtMsec = t.startedAtMsec;
    s.finishedAtMsec = t.finishedAtMsec;
    return s;
}

void RenderQueue::stop() {
    QMutexLocker locker(&m_mutex);
    m_stopped.store(true, std::memory_order_relaxed);
    m_doneCond.wakeAll();
}

void RenderQueue::setConcurrency(int n) {
    QMutexLocker locker(&m_mutex);
    m_concurrency = n > 0 ? n : 0;
}

RenderQueue::Stats RenderQueue::stats() const {
    QMutexLocker locker(&m_mutex);
    Stats s;
    s.total = int(m_tasks.size());
    s.evicted = m_evicted;
    for (const auto& t : m_tasks) {
        switch (t->state) {
            case TaskState::Pending:   s.pending++; break;
            case TaskState::Running:   s.running++; break;
            case TaskState::Succeeded: s.done++; break;
            case TaskState::Failed:    s.done++; break;
        }
    }
    // 渲染负载可观测字段。busy 取 running（一个 worker 接一个任务，二者一一对应）。
    s.concurrency = m_concurrency;
    s.busy = s.running;
    s.peakPending = m_peakPending;
    return s;
}

} // namespace seimi
