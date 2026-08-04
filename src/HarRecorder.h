// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CdpClient.h"
#include "HarStore.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>

namespace seimi {

class SsrfRequestInterceptor;

// HAR 网络录制中枢。
//
// 职责：
//   1. 为每个声明 recordHar 的渲染任务 attach 一个 CdpClient 到对应的 CDP page target，
//      订阅 Network domain 事件，按 requestId 聚合为 HAR 1.2 entry。
//   2. task 结束（page 销毁）时 detach，把聚合好的 HAR JSON 归档到 HarStore。
//   3. 接收 SsrfRequestInterceptor 的请求侧观测信号，作为 CDP 漏事件时的兜底/校验。
//
// 线程：全程 GUI 线程（CdpClient 走 QWebSocket 信号槽，不阻塞事件循环）。
// interceptor 的信号跨线程（IO→GUI）经 QueuedConnection 投递。
//
// targetId 发现：Qt6 QWebEnginePage 无公开 targetId API，用 HTTP GET /json 按 page URL 匹配。
class HarRecorder : public QObject {
    Q_OBJECT
public:
    HarRecorder(int debugPort, HarStore* store, QObject* parent = nullptr);
    ~HarRecorder() override;

    // 是否已启用（debug 端口 > 0 时构造为启用态）。
    bool isEnabled() const { return m_debugPort > 0; }

    // 在 RenderWorker::startAttempt 里调（page 创建后、load 前/后均可）：
    // 异步发现 targetId 并 attach CDP client。不会阻塞。
    void attachForTask(const QString& taskId, const QString& pageUrl);

    // 在 RenderWorker::destroyPage 里调（page 销毁前）：
    // 断开 CDP client，把已聚合的 entry 序列化为 HAR 归档。
    void detachForTask(const QString& taskId);

    // 绑定请求侧拦截器（interceptor 发 requestObserved 信号，连到本对象槽）。
    // 由 RenderPool::start 在装好 interceptor 后调。
    void bindInterceptor(SsrfRequestInterceptor* interceptor);

private slots:
    // 拦截器请求观测（跨线程 QueuedConnection 投递过来）。
    // 拦截器无法关联 taskId，按 URL 匹配活跃 session 的 entry 补全请求侧字段。
    void onInterceptorRequest(const QString& method,
                              const QString& url, const QHash<QString, QString>& headers,
                              const QString& firstPartyUrl, int resourceType);

private:
    // targetId 发现：HTTP GET /json 按 URL 匹配 type=="page" 的 target。
    // 并发安全：排除已被其他 session 占用的 target（m_usedTargetIds），避免多 task 连同一 page。
    QString discoverTargetId(const QString& pageUrl);

    // 单个录制会话（一个 task 对应一个 CdpClient + 一组 entry）。
    struct Session {
        std::unique_ptr<CdpClient> client;
        QString pageUrl;
        QString targetId;                                  // 已锁定的 CDP target id
        QHash<QString, class HarEntryBuilder*> entries;  // requestId -> builder
        bool finalized = false;
    };

    void setupEventHandlers(const QString& taskId, Session& s);
    void finalizeAndArchive(const QString& taskId);
    // targetId 发现重试（Chromium 异步创建 page target，首次查询可能查不到）。
    void retryDiscover(const QString& taskId, const QString& pageUrl, int attempt);

    int m_debugPort;
    QString m_debugHost = QStringLiteral("127.0.0.1");
    HarStore* m_store;
    // QHash 值需可拷贝，unique_ptr 不可拷贝；用 shared_ptr 表达 Session 所有权。
    // 逻辑上 Session 由本类独占（attach 创建、finalize 析构），shared_ptr 仅用于绕过 QHash 的拷贝要求。
    QHash<QString, std::shared_ptr<Session>> m_sessions;  // taskId -> session
    // 已被 session 占用的 CDP targetId 集合（并发隔离：多任务同时渲染时，各自 page target 互不串）。
    QSet<QString> m_usedTargetIds;
};

} // namespace seimi
