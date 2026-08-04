// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "CdpClient.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace seimi {

CdpClient::CdpClient(QObject* parent)
    : QObject(parent) {}

CdpClient::~CdpClient() {
    close();
}

bool CdpClient::connectToTarget(const QString& host, int port, const QString& targetId, bool waitForReady) {
    m_host = host;
    m_port = port;
    m_targetId = targetId;

    if (m_ws) {
        m_ws->deleteLater();
    }
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &CdpClient::onTextMessage);
    connect(m_ws, &QWebSocket::connected, this, &CdpClient::onWsConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &CdpClient::onWsDisconnected);
    connect(m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onWsError();
    });

    QUrl url(QStringLiteral("ws://%1:%2/devtools/page/%3")
                 .arg(host).arg(port).arg(targetId));
    m_ws->open(url);

    if (!waitForReady) return true;

    // 同步等 Network.enable 完成（connected 信号）。超时 2s 兜底。
    QEventLoop loop;
    bool ok = false;
    QMetaObject::Connection connOk = connect(this, &CdpClient::connected, this, [&]() { ok = true; loop.quit(); });
    QMetaObject::Connection connErr = connect(this, &CdpClient::errorOccurred, this, [&loop]() { loop.quit(); });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(connOk);
    disconnect(connErr);
    return ok;
}

void CdpClient::on(const QString& method, EventCallback cb) {
    m_handlers.insert(method, std::move(cb));
}

void CdpClient::send(const QString& method, const QJsonObject& params, CommandCallback cb) {
    if (!isConnected()) {
        if (cb) cb(QJsonObject(), false);
        return;
    }
    const int id = m_nextId++;
    if (cb) m_pending.insert(id, std::move(cb));

    QJsonObject req;
    req["id"] = id;
    req["method"] = method;
    req["params"] = params;
    m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)));
}

void CdpClient::close() {
    m_networkEnabled = false;
    if (m_ws) {
        disconnect(m_ws, &QWebSocket::disconnected, this, &CdpClient::onWsDisconnected);
        m_ws->close();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
    // 唤醒所有未完成的命令回调。
    const auto pending = m_pending;
    m_pending.clear();
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it.value()) it.value()(QJsonObject(), false);
    }
}

void CdpClient::onTextMessage(const QString& msg) {
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    dispatchMessage(doc.object());
}

void CdpClient::dispatchMessage(const QJsonObject& obj) {
    // 响应（有 id）：配对待响应回调。
    if (obj.contains("id")) {
        const int id = obj.value("id").toInt();
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        CommandCallback cb = it.value();
        m_pending.erase(it);
        if (cb) {
            const QJsonObject errObj = obj.value("error").toObject();
            const QJsonObject result = obj.value("result").toObject();
            cb(result, errObj.isEmpty());
        }
        return;
    }
    // 事件（有 method）：按 method 分发。
    const QString method = obj.value("method").toString();
    if (method.isEmpty()) return;
    auto it = m_handlers.find(method);
    if (it == m_handlers.end()) return;
    if (it.value()) it.value()(obj.value("params").toObject());
}

void CdpClient::onWsConnected() {
    // 连接建立后立即发 Network.enable，成功后才视为真正就绪。
    // maxPostDataSize=-1：尽量带上完整 POST body（HAR postData 需要）。
    QJsonObject params;
    params["maxPostDataSize"] = -1;
    send(QStringLiteral("Network.enable"), params,
         [this](const QJsonObject&, bool ok) {
             m_networkEnabled = ok;
             if (ok) {
                 emit connected();
             } else {
                 emit errorOccurred(QStringLiteral("Network.enable failed"));
             }
         });
}

void CdpClient::onWsDisconnected() {
    m_networkEnabled = false;
    emit disconnected();
}

void CdpClient::onWsError() {
    const QString reason = m_ws ? m_ws->errorString() : QStringLiteral("unknown");
    emit errorOccurred(reason);
}

} // namespace seimi
