// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

class QNetworkAccessManager;

namespace seimi {

class ProxyConfig;

// /fetch 直取的请求/结果载体（HttpServer 组装 → FetchService 执行）。
struct FetchRequest {
    QUrl url;
    QString method = QStringLiteral("GET");
    std::vector<std::pair<QByteArray, QByteArray>> headers;  // 已含默认 UA（调用方可覆盖）
    QByteArray body;      // POST/PUT/PATCH 请求体
    int timeoutMsec = 15000;
};

struct FetchResult {
    bool ok = false;      // 传输层成功（拿到 HTTP 响应；4xx/5xx 也算 ok，看 statusCode）
    bool timedOut = false;
    QString error;        // 传输层错误描述（ok=false 时有意义）
    int statusCode = 0;
    QString finalUrl;     // 重定向后的最终地址
    std::vector<std::pair<QByteArray, QByteArray>> responseHeaders;
    QByteArray body;      // 完整响应体（解码后；上限裁剪由 HttpServer 按输出模式决定）
};

// /fetch 的专职执行器：一个长命 QNetworkAccessManager 跑在专属 QThread（自带事件循环），
// httplib 工作线程经 execute() 线程安全桥同步等待结果。
//
// 为什么不能用「httplib 线程栈上 QNAM + 栈上 QEventLoop」（v1.3.0 实撞两次崩溃，教训固化）：
//  1) adopted 线程的事件分发器由首次 exec 惰性创建、永不销毁；栈上 QNAM 析构后，其内部
//     连接池/定时器残留的队列事件在下一次同线程 exec 时可能命中已释放的对象；
//  2) 小 JSON 响应常侥幸通过，数 MB 二进制附件场景稳定 SIGSEGV（真实使用撞过）；
//  3) 每请求新建/销毁 QNAM 本身就是反模式（Qt 官方要求 QNAM 长命且少实例）。
// 本设计：QNAM 只建一次、活在有事件循环的正规 QThread 里；回复级状态机全异步
//（finished 回调交付），天然支持并发请求；httplib 线程只用 std 原语等待，零 Qt 依赖。
class FetchWorker : public QObject {
    Q_OBJECT
public:
    // proxy 可空；bypass 模式下给 QNAM 挂实例级分流 factory（见 ProxyConfig 类注释）。
    explicit FetchWorker(ProxyConfig* proxy, QObject* parent = nullptr);

public slots:
    // 异步发起请求后立即返回；完成/超时经 done 信号交付（全在 worker 线程内）。
    void run(seimi::FetchRequest req, quint64 jobId);

signals:
    void done(quint64 jobId, seimi::FetchResult result);

private:
    QNetworkAccessManager* m_nam = nullptr;  // 懒建一次（必须在 worker 线程创建）
    ProxyConfig* m_proxy;
};

class FetchService {
public:
    explicit FetchService(ProxyConfig* proxy);
    ~FetchService();

    // 阻塞执行一次直取（任意线程可调；内部经条件变量等待，不占用/不依赖调用方线程的
    // Qt 事件循环）。等待上限 = timeoutMsec + 15s 管理余量（worker 内部 abort 兜底必达
    // 终态；兜底超时视为内部异常，返回明确错误而非永久挂起调用方）。
    FetchResult execute(const FetchRequest& req);

private:
    QThread m_thread;
    FetchWorker* m_worker = nullptr;
    std::atomic<quint64> m_nextJobId{1};
};

} // namespace seimi

Q_DECLARE_METATYPE(seimi::FetchRequest)
Q_DECLARE_METATYPE(seimi::FetchResult)
