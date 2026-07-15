// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "RenderQueue.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QWebSocket>
#include <QWebSocketServer>

namespace seimi {

// WebSocket 服务：支持「提交渲染请求」与「订阅任务完成推送」。
//
// 客户端消息（JSON，用 action 区分）：
//   {"action":"render","url":"...","settle_ms":2500}     提交渲染（自动订阅本任务）
//   {"action":"subscribe","task_id":"<id>"}              订阅已存在任务
//   {"action":"auth","token":"<token>"}                  鉴权（启用密码且连接 URL 未带 ?token= 时）
//
// 服务端消息（用 event 区分）：
//   {"event":"created","task_id":"..."}
//   {"event":"subscribed","task_id":"..."}
//   {"event":"finished","task_id":"...","state":"succeeded|failed"}  （主动推送）
//   {"event":"authorized"}
//   {"event":"error","message":"..."}
//
// 鉴权：构造时传 token（空=不启用）。启用后连接须通过鉴权才能发 render/subscribe：
//   1) 连接 URL query：ws://host:port/?token=<token>
//   2) 首条消息 {"action":"auth","token":"<token>"}
//   与 HTTP 同一确定性 token（HttpServer::computeToken(password)）。
//
// 全部运行在 GUI 主线程事件循环（QWebSocketServer 信号槽）。
class WsServer : public QObject {
    Q_OBJECT
public:
    // queue: 用于提交渲染请求；settleDefaultMs: render 未指定 settle_ms 时的默认值。
    // authToken: 非空时启用鉴权（与 HttpServer 同一 token）。
    WsServer(RenderQueue* queue, int settleDefaultMs, std::string authToken = std::string(),
             QObject* parent = nullptr);

    // 在指定地址监听。必须与 HttpServer 的 --host 一致（默认 127.0.0.1，
    // 避免在所有网卡暴露无密码的渲染/SSRF 入口）。
    bool listen(const QHostAddress& address, quint16 port);

    quint16 serverPort() const;

public slots:
    // 主线程收到 RenderPool::taskFinished 时调用，向所有订阅者推送 finished。
    void notifyFinished(const QString& taskId, const QString& state);

private slots:
    void onNewConnection();
    void onTextMessageReceived(const QString& text);
    void onSocketDisconnected();

private:
    // 处理 render 与 subscribe 两种 action。返回是否已识别处理。
    void handleRender(QWebSocket* sock, const QJsonObject& obj);
    void handleSubscribe(QWebSocket* sock, const QString& taskId);
    // 把 sock 订阅到 taskId。一个连接可同时关注多个任务（render 连发 / render+subscribe 混用），
    // 故只增不删——新订阅不会解除对该连接旧任务的订阅。用于 render/subscribe 复用。
    void subscribe(QWebSocket* sock, const QString& taskId);
    // 鉴权：sock 是否已通过鉴权（未启用密码时恒为 true）。
    bool isAuthorized(QWebSocket* sock) const;

    QWebSocketServer* m_server;
    RenderQueue* m_queue;
    int m_settleDefaultMs;
    std::string m_authToken;        // 空=不启用鉴权
    // 已通过鉴权的 socket 集合（启用密码时用）。未启用密码时不查此集合。
    QSet<QWebSocket*> m_authorized;

    // 反向索引：task_id -> 订阅该任务的 socket 列表
    QHash<QString, QList<QWebSocket*>> m_subscriptions;
    // socket -> 它订阅的所有 task_id 集合（一个连接可同时关注多个任务；便于断开时清理）
    QHash<QWebSocket*, QSet<QString>> m_socketToTask;
};

} // namespace seimi
