// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QWebSocket>

#include <QHash>

#include <functional>

namespace seimi {

// 最小 CDP（Chrome DevTools Protocol）客户端。
//
// 用途：连本进程 Qt WebEngine 通过 --remote-debugging-port 开出的 CDP 端点，
// 订阅 Network domain 事件用于 HAR 网络录制。
//
// 设计：
//   - 全程跑在 GUI 线程（QWebSocket 信号槽驱动，不阻塞事件循环）。
//   - JSON-RPC over WebSocket：自增 id 配对请求/响应；事件按 method 字符串分发。
//   - 连接的是 page-level target：ws://127.0.0.1:<port>/devtools/page/<targetId>。
//
// 不依赖任何第三方 CDP 库——Network domain 的几个命令 + 事件就够 HAR 录制用。
class CdpClient : public QObject {
    Q_OBJECT
public:
    using EventCallback = std::function<void(const QJsonObject& params)>;
    using CommandCallback = std::function<void(const QJsonObject& result, bool ok)>;

    explicit CdpClient(QObject* parent = nullptr);
    ~CdpClient() override;

    // 连接指定 targetId 的 page-level CDP 端点。host 通常 "127.0.0.1"。
    // 同步阻塞直到 Network.enable 完成（或超时/失败），确保后续 load() 的请求能被捕获。
    // waitForReady=true 时阻塞等 connected 信号（约 50-200ms localhost 往返），适合 startAttempt
    // 里在 load() 前调用，保证 CDP 事件流就绪后才发起导航。
    // 返回是否成功就绪（connected 且 Network.enable 成功）。
    bool connectToTarget(const QString& host, int port, const QString& targetId, bool waitForReady = true);

    // 注册 CDP 事件处理器（按 method 全字符串匹配，如 "Network.requestWillBeSent"）。
    void on(const QString& method, EventCallback cb);

    // 发送 CDP 命令，异步回调。ok=false 表示 CDP 层返回 error 或连接已断。
    void send(const QString& method, const QJsonObject& params, CommandCallback cb);

    // 主动断开（page 销毁前调；target 会随 page 自动消失，无需 Target.closeTarget）。
    void close();

    bool isConnected() const { return m_ws && m_ws->isValid(); }

signals:
    // Network.enable 成功后发出，表示事件流已就绪。
    void connected();
    // 连接断开（对端关闭 / 主动 close / 出错）。
    void disconnected();
    // 连接/协议错误（带可读原因，供日志）。
    void errorOccurred(const QString& reason);

private slots:
    void onTextMessage(const QString& msg);
    void onWsConnected();
    void onWsDisconnected();
    void onWsError();

private:
    void dispatchMessage(const QJsonObject& obj);

    QWebSocket* m_ws = nullptr;
    QString m_host;
    int m_port = 0;
    QString m_targetId;
    int m_nextId = 1;   // JSON-RPC id 自增
    bool m_networkEnabled = false;

    // id -> 待响应回调
    QHash<int, CommandCallback> m_pending;
    // method -> 事件回调
    QHash<QString, EventCallback> m_handlers;
};

} // namespace seimi
