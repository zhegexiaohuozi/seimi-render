// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "WsServer.h"

#include "TokenCompare.h"
#include "UrlGuard.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>

namespace seimi {

static qint64 nowMsec() { return QDateTime::currentMSecsSinceEpoch(); }

// 统一用 QJsonDocument 构造消息，避免字段值含特殊字符时破坏 JSON。
static void sendJson(QWebSocket* sock, const QJsonObject& obj) {
    if (sock && sock->isValid()) {
        sock->sendTextMessage(QString::fromUtf8(
            QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }
}

WsServer::WsServer(RenderQueue* queue, int settleDefaultMs, std::string authToken,
                   QObject* parent)
    : QObject(parent)
    , m_server(new QWebSocketServer(QStringLiteral("seimi-ws"),
                                    QWebSocketServer::NonSecureMode, this))
    , m_queue(queue)
    , m_settleDefaultMs(settleDefaultMs)
    , m_authToken(std::move(authToken)) {
    connect(m_server, &QWebSocketServer::newConnection,
            this, &WsServer::onNewConnection);
}

bool WsServer::isAuthorized(QWebSocket* sock) const {
    // 未启用密码：恒为放行（与 HttpServer 语义一致）。
    if (m_authToken.empty()) return true;
    return m_authorized.contains(sock);
}

bool WsServer::listen(const QHostAddress& address, quint16 port) {
    return m_server->listen(address, port);
}

quint16 WsServer::serverPort() const {
    return m_server->serverPort();
}

void WsServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QWebSocket* sock = m_server->nextPendingConnection();
        connect(sock, &QWebSocket::textMessageReceived,
                this, &WsServer::onTextMessageReceived);
        connect(sock, &QWebSocket::disconnected,
                this, &WsServer::onSocketDisconnected);
        m_socketToTask.insert(sock, {}); // 暂未订阅任何任务

        // 启用密码时优先用连接 URL ?token= 鉴权。失败不立即关连接，给客户端首条消息 auth 的机会。
        if (!m_authToken.empty()) {
            QUrlQuery q(sock->requestUrl());
            QString tok = q.queryItemValue(QStringLiteral("token"));
            if (!tok.isEmpty()
                && constantTimeEquals(tok.toStdString(), m_authToken)) {
                m_authorized.insert(sock);
                sendJson(sock, {{QStringLiteral("event"), QStringLiteral("authorized")}});
            }
        }
    }
}

void WsServer::onTextMessageReceived(const QString& text) {
    auto* sock = qobject_cast<QWebSocket*>(sender());
    if (!sock) return;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"), QStringLiteral("invalid json")}});
        return;
    }
    QJsonObject obj = doc.object();
    QString action = obj.value(QStringLiteral("action")).toString();

    // auth 动作可以在任意时刻发送（未鉴权时唯一被接受的非 render/subscribe 动作）。
    if (action == QStringLiteral("auth")) {
        if (m_authToken.empty()) {
            // 未启用密码时 auth 是无意义操作，告知客户端已放行。
            sendJson(sock, {{QStringLiteral("event"), QStringLiteral("authorized")}});
            return;
        }
        QString tok = obj.value(QStringLiteral("token")).toString();
        if (!tok.isEmpty() && constantTimeEquals(tok.toStdString(), m_authToken)) {
            m_authorized.insert(sock);
            sendJson(sock, {{QStringLiteral("event"), QStringLiteral("authorized")}});
        } else {
            sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                            {QStringLiteral("message"), QStringLiteral("invalid token")}});
            // 鉴权失败：关闭连接。closeCode 1008 = Policy Violation。
            sock->close(QWebSocketProtocol::CloseCodePolicyViolated,
                        QStringLiteral("unauthorized"));
        }
        return;
    }

    // 启用密码时，render/subscribe 必须先通过鉴权，否则拒绝并关闭。
    if (!isAuthorized(sock)) {
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"),
                         QStringLiteral("unauthorized; send {\"action\":\"auth\",\"token\":\"...\"} or connect with ?token=")}});
        sock->close(QWebSocketProtocol::CloseCodePolicyViolated,
                    QStringLiteral("unauthorized"));
        return;
    }

    if (action == QStringLiteral("render")) {
        handleRender(sock, obj);
    } else if (action == QStringLiteral("subscribe")) {
        QString taskId = obj.value(QStringLiteral("task_id")).toString();
        if (taskId.isEmpty()) {
            sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                            {QStringLiteral("message"), QStringLiteral("missing 'task_id'")}});
            return;
        }
        handleSubscribe(sock, taskId);
    } else {
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"),
                         QStringLiteral("unknown action; expect 'render' or 'subscribe'")}});
    }
}

void WsServer::handleRender(QWebSocket* sock, const QJsonObject& obj) {
    QString url = obj.value(QStringLiteral("url")).toString().trimmed();
    if (url.isEmpty()) {
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"), QStringLiteral("missing 'url'")}});
        return;
    }
    // URL 基础校验，与 HTTP /render 保持一致。
    QUrl qurl(url);
    if (!qurl.isValid() || (qurl.scheme() != QStringLiteral("http")
                            && qurl.scheme() != QStringLiteral("https"))) {
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"), QStringLiteral("url must be http/https")}});
        return;
    }
    // SSRF 防护，与 HTTP /render 保持一致：拒绝内网/回环/链路本地/元数据地址。
    std::string ssrfReason = urlSsrfCheck(qurl);
    if (!ssrfReason.empty()) {
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"),
                         QStringLiteral("url blocked by SSRF guard: ")
                             + QString::fromStdString(ssrfReason)}});
        return;
    }

    int settleMs = int(obj.value(QStringLiteral("settle_ms")).toInt(m_settleDefaultMs));
    settleMs = std::clamp(settleMs, 0, 30000); // 与 HTTP 接口相同的上限

    // 解析 output（与 HTTP /render 同语义）。默认 html。
    OutputMask outputs = static_cast<OutputMask>(Output::Html);
    {
        QJsonValue v = obj.value(QStringLiteral("output"));
        OutputMask m = 0;
        auto add = [&](const QString& tok) {
            QString t = tok.trimmed().toLower();
            if (t == QStringLiteral("html"))     m |= static_cast<OutputMask>(Output::Html);
            else if (t == QStringLiteral("markdown") || t == QStringLiteral("md"))
                                              m |= static_cast<OutputMask>(Output::Markdown);
            else if (t == QStringLiteral("pdf"))  m |= static_cast<OutputMask>(Output::Pdf);
            else if (t == QStringLiteral("screenshot") || t == QStringLiteral("image")
                     || t == QStringLiteral("png") || t == QStringLiteral("jpg"))
                                              m |= static_cast<OutputMask>(Output::Screenshot);
        };
        if (v.isString()) for (const auto& t : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) add(t);
        else if (v.isArray()) for (const auto& e : v.toArray()) if (e.isString()) add(e.toString());
        if (m) outputs = m;
    }

    // 截图编码格式：png/jpg/jpeg/auto（与 HTTP /render 同语义）。默认 auto。
    ImageFormat imgFmt = ImageFormat::Auto;
    {
        QString t = obj.value(QStringLiteral("format")).toString().trimmed().toLower();
        if (t == QStringLiteral("png"))            imgFmt = ImageFormat::Png;
        else if (t == QStringLiteral("jpg") || t == QStringLiteral("jpeg"))
                                                 imgFmt = ImageFormat::Jpeg;
    }
    // markdown 正文提取算法：conservative/readability（与 HTTP 同语义）。默认 conservative。
    MdAlgorithm mdAlg = MdAlgorithm::Conservative;
    {
        QString t = obj.value(QStringLiteral("md_algorithm")).toString().trimmed().toLower();
        if (t == QStringLiteral("readability"))   mdAlg = MdAlgorithm::Readability;
    }

    // 提交到渲染队列（线程安全）。render 请求自动订阅本任务，
    // 因此后续 finished 推送会通过 notifyFinished 回到这个连接。
    QString taskId = m_queue->submit(url, settleMs, outputs, imgFmt, mdAlg, nowMsec());
    if (taskId.isEmpty()) {
        // 任务表过载（洪泛提交/积压）：背压拒绝，让客户端稍后重试。
        sendJson(sock, {{QStringLiteral("event"), QStringLiteral("error")},
                        {QStringLiteral("message"), QStringLiteral("server overloaded, retry later")}});
        return;
    }
    subscribe(sock, taskId);

    sendJson(sock, {{QStringLiteral("event"), QStringLiteral("created")},
                    {QStringLiteral("task_id"), taskId},
                    {QStringLiteral("url"), url}});
    // 注：不在此回 finished——渲染未完成。完成后由 notifyFinished 推送。
}

void WsServer::handleSubscribe(QWebSocket* sock, const QString& taskId) {
    subscribe(sock, taskId);
    sendJson(sock, {{QStringLiteral("event"), QStringLiteral("subscribed")},
                    {QStringLiteral("task_id"), taskId}});
}

void WsServer::subscribe(QWebSocket* sock, const QString& taskId) {
    // 一个连接可同时关注多个任务（render 连发 / render+subscribe 混用），
    // 故只增不删——新订阅不会解除旧任务订阅。
    QSet<QString>& tasks = m_socketToTask[sock];
    // 已订阅过该 task：无需重复登记。
    if (!tasks.contains(taskId)) {
        tasks.insert(taskId);
        QList<QWebSocket*>& subs = m_subscriptions[taskId];
        if (!subs.contains(sock)) subs.append(sock);
    }
}

void WsServer::onSocketDisconnected() {
    auto* sock = qobject_cast<QWebSocket*>(sender());
    if (!sock) return;

    // 一个连接可能同时订阅多个任务，逐个从对应列表移除并清理空列表。
    for (const QString& taskId : m_socketToTask.value(sock)) {
        QList<QWebSocket*>& subs = m_subscriptions[taskId];
        subs.removeAll(sock);
        if (subs.isEmpty()) m_subscriptions.remove(taskId);
    }
    m_socketToTask.remove(sock);
    m_authorized.remove(sock);
    sock->deleteLater();
}

void WsServer::notifyFinished(const QString& taskId, const QString& state, bool blocked) {
    auto it = m_subscriptions.find(taskId);
    if (it == m_subscriptions.end()) return;

    QJsonObject msg{
        {QStringLiteral("event"), QStringLiteral("finished")},
        {QStringLiteral("task_id"), taskId},
        {QStringLiteral("state"), state}};
    if (blocked) msg.insert(QStringLiteral("blocked"), true);   // 反爬拦截类型化标记

    // 拷贝列表：发送可能触发对端关闭 → 修改原列表
    QList<QWebSocket*> subs = it.value();
    for (QWebSocket* s : subs) {
        sendJson(s, msg);
    }
}

} // namespace seimi
