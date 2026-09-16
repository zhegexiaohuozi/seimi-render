// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "FetchService.h"

#include "ProxyConfig.h"

#include <QMetaType>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <chrono>

namespace seimi {

// 等待帧注册表：jobId -> 调用方栈上的等待帧。execute 注册/注销，deliverResult 查找写回。
// 进程级低争用（每次 fetch 两次短锁）。WaitFrame 是 execute 的栈结构，为保持头文件
// 纯净（不暴露条件变量等内部件），此处用文件内等价类型 + 类型安全注册表实现。
namespace {

struct FrameBase {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    FetchResult result;
};

std::mutex g_registryMutex;
std::map<quint64, FrameBase*> g_registry;

void deliverResult(quint64 jobId, const FetchResult& r) {
    FrameBase* frame = nullptr;
    {
        std::lock_guard<std::mutex> g(g_registryMutex);
        auto it = g_registry.find(jobId);
        if (it == g_registry.end()) return;  // 调用方已超时返回并注销，丢弃迟到结果
        frame = it->second;
    }
    {
        std::lock_guard<std::mutex> g(frame->mutex);
        frame->result = r;
        frame->ready = true;
    }
    frame->cv.notify_all();
}

} // namespace

// ============================================================
// FetchWorker（全部在 worker 线程内运行）
// ============================================================

FetchWorker::FetchWorker(ProxyConfig* proxy, QObject* parent)
    : QObject(parent), m_proxy(proxy) {}

void FetchWorker::run(seimi::FetchRequest req, quint64 jobId) {
    // QNAM 必须在本线程创建且长命（跨线程 not allowed；每请求新建是崩溃根源之一）。
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        // bypass 模式：/fetch 专属 per-host 分流 factory（实例级，不动全局——全局注册会让
        // QtWebEngine 轮询推送覆盖 Chromium flag，见 ProxyConfig 类注释）。
        if (m_proxy && m_proxy->bypassEnabled()) {
            m_nam->setProxyFactory(m_proxy->fetchProxyFactory());
        }
    }

    QNetworkRequest nreq(req.url);
    // 传输层超时兜底：与业务级 QTimer 双保险（任一触发都会 abort → finished）。
    nreq.setTransferTimeout(static_cast<int>(req.timeoutMsec));
    for (const auto& h : req.headers) {
        nreq.setRawHeader(h.first, h.second);
    }

    QNetworkReply* reply = nullptr;
    const QString m = req.method.toUpper();
    if (m == QStringLiteral("GET"))            reply = m_nam->get(nreq);
    else if (m == QStringLiteral("HEAD"))      reply = m_nam->head(nreq);
    else if (m == QStringLiteral("POST"))      reply = m_nam->post(nreq, req.body);
    else if (m == QStringLiteral("PUT"))       reply = m_nam->put(nreq, req.body);
    else if (m == QStringLiteral("PATCH"))     reply = m_nam->sendCustomRequest(nreq, "PATCH", req.body);
    else if (m == QStringLiteral("DELETE"))    reply = m_nam->deleteResource(nreq);
    else {
        // 上游已校验 method，此处防御性兜底。
        FetchResult r;
        r.error = QStringLiteral("unsupported method: ") + req.method;
        emit done(jobId, r);
        return;
    }

    // 业务级超时：abort 触发 finished（OperationCanceledError → timedOut 语义）。
    auto* timer = new QTimer(reply);   // 挂在 reply 上随其销毁
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, jobId, reply]() {
        FetchResult r;
        const bool canceled = reply->error() == QNetworkReply::OperationCanceledError;
        r.timedOut = canceled;
        r.ok = reply->error() == QNetworkReply::NoError;
        r.error = reply->errorString();
        r.statusCode = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.finalUrl = reply->url().toString();
        for (const QByteArray& h : reply->rawHeaderList()) {
            r.responseHeaders.emplace_back(h, reply->rawHeader(h));
        }
        if (!canceled) {
            r.body = reply->readAll();
        }
        reply->deleteLater();   // worker 线程事件循环处理 deferred delete，安全
        emit done(jobId, r);
    });
    timer->start(static_cast<int>(req.timeoutMsec));
}

// ============================================================
// FetchService（构造/析构在属主线程；execute 任意线程）
// ============================================================

FetchService::FetchService(ProxyConfig* proxy)
    : m_thread() {
    // 队列投递 FetchRequest/FetchResult 需要注册元类型（跨线程信号/invokeMethod）。
    qRegisterMetaType<seimi::FetchRequest>("seimi::FetchRequest");
    qRegisterMetaType<seimi::FetchResult>("seimi::FetchResult");

    m_worker = new FetchWorker(proxy);   // 无父，moveToThread 后生命周期由本类管理
    m_worker->moveToThread(&m_thread);
    // done → deliverResult：DirectConnection 回调在 emit 线程（worker 线程）执行，
    // 直接写回等待帧——不经过任何对象所属线程的事件队列。
    QObject::connect(m_worker, &FetchWorker::done, m_worker,
                     [](quint64 jobId, const seimi::FetchResult& r) {
                         deliverResult(jobId, r);
                     },
                     Qt::DirectConnection);
    QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

FetchService::~FetchService() {
    m_thread.quit();
    m_thread.wait();
}

FetchResult FetchService::execute(const FetchRequest& req) {
    FrameBase frame;
    const quint64 jobId = m_nextJobId.fetch_add(1);
    {
        std::lock_guard<std::mutex> g(g_registryMutex);
        g_registry[jobId] = &frame;
    }
    QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection,
                              Q_ARG(seimi::FetchRequest, req),
                              Q_ARG(quint64, jobId));

    // 等待上限：业务超时 + 15s 管理余量。worker 的 abort/transferTimeout 双兜底保证
    // 必达终态；若仍超此限说明内部状态异常（如 worker 线程卡死），返回明确错误而非
    // 永久占用 httplib 线程。
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(req.timeoutMsec + 15000);
    std::unique_lock<std::mutex> lk(frame.mutex);
    const bool signaled = frame.cv.wait_until(lk, deadline, [&frame] { return frame.ready; });
    lk.unlock();
    {
        std::lock_guard<std::mutex> g(g_registryMutex);
        g_registry.erase(jobId);   // 迟到的 deliverResult 会发现条目已注销并安全丢弃
    }
    if (!signaled) {
        FetchResult r;
        r.timedOut = true;
        r.error = QStringLiteral("fetch service internal timeout (worker unresponsive)");
        return r;
    }
    return frame.result;
}

} // namespace seimi
