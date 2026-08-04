// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "HarRecorder.h"
#include "SsrfInterceptor.h"

#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <httplib.h>

#include <vector>

#ifndef SEIMI_VERSION
#define SEIMI_VERSION "unknown"
#endif

namespace seimi {

// ============================================================
// HarEntryBuilder：按 requestId 累积单个 HAR entry 的各阶段数据。
// ============================================================
class HarEntryBuilder {
public:
    // request 阶段（来自 Network.requestWillBeSent）
    QString requestId;
    QString method;
    QString url;
    QString httpVersionReq = QStringLiteral("HTTP/1.1");
    QHash<QString, QString> reqHeaders;
    QString postData;
    QString postMime;
    bool hasPostData = false;
    double wallTime = 0.0;          // Unix epoch 秒（wall clock）
    double cdpTimestamp = 0.0;      // monotonic（用于算 time）
    QString resourceType;           // 小写：xhr/document/script/...
    QJsonObject initiator;          // 原样透传到 _initiator

    // response 阶段（来自 Network.responseReceived）
    bool hasResponse = false;
    int status = 0;
    QString statusText;
    QString httpVersionResp = QStringLiteral("HTTP/1.1");
    QHash<QString, QString> respHeaders;
    QString mimeType;
    QString remoteIp;
    int connectionId = 0;
    QJsonObject timing;             // CDP ResourceTiming 原始
    double respTimestamp = 0.0;     // responseReceived 的 monotonic 时间
    qint64 encodedDataLength = -1;  // 线上传输字节数

    // body（来自 Network.getResponseBody）
    QString body;
    bool bodyIsBase64 = false;
    bool bodyFetched = false;
    qint64 decodedBodySize = -1;

    // 结束
    bool finished = false;
    bool failed = false;
    QString errorText;
    double finishedTimestamp = 0.0;

    // 兜底（来自拦截器）
    bool interceptedReqSeen = false;

    QJsonObject buildHarEntry() const;
};

// CDP protocol（如 "h2"/"http/1.1"/"quic"）归一为 HAR httpVersion。
static QString normalizeHttpVersion(const QString& proto) {
    const QString p = proto.toLower();
    if (p == QStringLiteral("h2") || p == QStringLiteral("http/2")) return QStringLiteral("HTTP/2");
    if (p == QStringLiteral("h3") || p == QStringLiteral("quic")) return QStringLiteral("HTTP/3");
    if (p == QStringLiteral("http/1.1")) return QStringLiteral("HTTP/1.1");
    if (p == QStringLiteral("http/1.0")) return QStringLiteral("HTTP/1.0");
    return proto.isEmpty() ? QStringLiteral("HTTP/1.1") : proto;
}

// CDP ResourceType（如 "XHR"/"Script"）转小写（Chrome HAR 约定小写）。
static QString resourceTypeLower(const QString& t) {
    QString s = t.toLower();
    // CDP 的 "Stylesheet" 在 HAR 里习惯写 "stylesheet"（已是），保持。
    // "Fetch" 归类为 "fetch"。其余直接小写。
    return s;
}

// timings 算法（参照 chrome-har）：CDP timing 字段是相对 requestTime 的秒级偏移。
// 输出 HAR 必填的 send/wait/receive（毫秒，-1 表示未知/未发生）。
static void computeTimings(const QJsonObject& timing,
                           double sendStart, double sendEnd,
                           double receiveHeadersEnd,
                           double respTs, double finishedTs,
                           double& outSend, double& outWait, double& outReceive,
                           double& outTotal) {
    // send：sendEnd - sendStart（秒转毫秒）。
    if (sendEnd > 0 && sendStart >= 0) {
        outSend = (sendEnd - sendStart) * 1000.0;
    } else {
        outSend = -1;
    }
    // wait：receiveHeadersEnd - sendEnd。
    if (receiveHeadersEnd > 0 && sendEnd > 0) {
        outWait = (receiveHeadersEnd - sendEnd) * 1000.0;
    } else {
        outWait = -1;
    }
    // receive：用 finishedTs - respTs 兜底（response 到 loadingFinished 的窗口）。
    if (finishedTs > 0 && respTs > 0) {
        outReceive = (finishedTs - respTs) * 1000.0;
    } else {
        outReceive = -1;
    }
    // 负值修正（CDP 偶发给出负偏移）。
    auto clamped = [](double v) { return v < 0 ? -1.0 : v; };
    outSend = clamped(outSend);
    outWait = clamped(outWait);
    outReceive = clamped(outReceive);
    // total = send + wait + receive（保证自洽过 HAR schema：time == timings 非负项之和）。
    double sum = 0;
    if (outSend > 0) sum += outSend;
    if (outWait > 0) sum += outWait;
    if (outReceive > 0) sum += outReceive;
    outTotal = sum;
}

static QJsonArray headersToJsonArray(const QHash<QString, QString>& headers) {
    QJsonArray arr;
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        QJsonObject h;
        h["name"] = it.key();
        h["value"] = it.value();
        arr.append(h);
    }
    return arr;
}

static QJsonArray queryStringToJsonArray(const QString& url) {
    QJsonArray arr;
    const QUrl u(url);
    const QUrlQuery q(u);
    for (const auto& pair : q.queryItems()) {
        QJsonObject item;
        item["name"] = pair.first;
        item["value"] = pair.second;
        arr.append(item);
    }
    return arr;
}

QJsonObject HarEntryBuilder::buildHarEntry() const {
    // —— timings ——
    const QJsonObject t = timing;
    double outSend, outWait, outReceive, outTotal;
    computeTimings(t,
                   t.value("sendStart").toDouble(-1),
                   t.value("sendEnd").toDouble(-1),
                   t.value("receiveHeadersEnd").toDouble(-1),
                   respTimestamp, finishedTimestamp,
                   outSend, outWait, outReceive, outTotal);

    // startedDateTime：wallTime（Unix epoch 秒）→ ISO8601 带毫秒。
    QString startedIso;
    if (wallTime > 0) {
        const qint64 secs = static_cast<qint64>(wallTime);
        const int msec = static_cast<int>((wallTime - static_cast<double>(secs)) * 1000.0);
        startedIso = QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC)
                         .addMSecs(msec)
                         .toString(Qt::ISODateWithMs);
    } else {
        startedIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    }

    // —— request ——
    QJsonObject request;
    request["method"] = method.isEmpty() ? QStringLiteral("GET") : method;
    request["url"] = url;
    request["httpVersion"] = httpVersionReq;
    request["cookies"] = QJsonArray();   // 最小实现：空数组（schema 要求存在）
    request["headers"] = headersToJsonArray(reqHeaders);
    request["queryString"] = queryStringToJsonArray(url);
    request["headersSize"] = -1;          // 精确值需 ExtraInfo.headersText，最小实现 -1
    qint64 bodySize = 0;
    if (hasPostData && !postData.isEmpty()) {
        QJsonObject pd;
        pd["mimeType"] = postMime.isEmpty() ? QStringLiteral("application/octet-stream") : postMime;
        pd["text"] = postData;
        request["postData"] = pd;
        bodySize = postData.toUtf8().size();
    }
    request["bodySize"] = bodySize;

    // —— response ——
    QJsonObject response;
    response["status"] = status;
    response["statusText"] = statusText;
    response["httpVersion"] = httpVersionResp;
    response["cookies"] = QJsonArray();
    response["headers"] = headersToJsonArray(respHeaders);
    QJsonObject content;
    const qint64 contentSize = bodyFetched ? (decodedBodySize >= 0 ? decodedBodySize
                                                                   : static_cast<qint64>(body.toUtf8().size()))
                                           : (encodedDataLength >= 0 ? encodedDataLength : -1);
    content["size"] = contentSize;
    content["mimeType"] = mimeType.isEmpty() ? QStringLiteral("application/octet-stream") : mimeType;
    if (bodyFetched && !body.isEmpty() && contentSize != 0) {
        content["text"] = body;
        if (bodyIsBase64) content["encoding"] = QStringLiteral("base64");
    }
    response["content"] = content;
    response["redirectURL"] = respHeaders.value(QStringLiteral("Location"), QString());  // 空串即无重定向
    response["headersSize"] = -1;
    response["bodySize"] = encodedDataLength >= 0 ? encodedDataLength : 0;

    // —— timings ——
    QJsonObject timings;
    timings["send"] = outSend;
    timings["wait"] = outWait;
    timings["receive"] = outReceive;

    // —— 组装 entry ——
    QJsonObject entry;
    entry["startedDateTime"] = startedIso;
    entry["time"] = outTotal;
    entry["request"] = request;
    entry["response"] = response;
    entry["cache"] = QJsonObject();   // 必填但可空
    entry["timings"] = timings;

    // —— Chrome/Charles 扩展字段（_ 前缀，DevTools/Charles 体验质变）——
    if (!resourceType.isEmpty()) entry["_resourceType"] = resourceType;
    if (!initiator.isEmpty()) entry["_initiator"] = initiator;
    if (encodedDataLength >= 0) entry["_transferSize"] = static_cast<double>(encodedDataLength);
    if (!remoteIp.isEmpty()) entry["serverIPAddress"] = remoteIp;
    if (connectionId > 0) entry["connection"] = QString::number(connectionId);

    return entry;
}

// ============================================================
// HarRecorder
// ============================================================

HarRecorder::HarRecorder(int debugPort, HarStore* store, QObject* parent)
    : QObject(parent)
    , m_debugPort(debugPort)
    , m_store(store) {}

HarRecorder::~HarRecorder() {
    // 析构时把所有未归档的会话收尾。
    const auto taskIds = m_sessions.keys();
    for (const QString& id : taskIds) {
        finalizeAndArchive(id);
    }
}

void HarRecorder::bindInterceptor(SsrfRequestInterceptor* interceptor) {
    if (!interceptor) return;
    // 拦截器跑在 Chromium IO 线程，回调内必须快。用 QMetaObject::invokeMethod 把
    // 数据投递到 GUI 线程（QueuedConnection），IO 线程不阻塞。
    // this 指针在 HarRecorder 生命周期内稳定（构造后到 main 退出才销毁）。
    HarRecorder* self = this;
    interceptor->setRequestObserver([self](const InterceptorRequestInfo& ri) {
        // 拷贝数据后跨线程投递（QMetaObject::invokeMethod 的 lambda 重载走 QueuedConnection）。
        const QString method = QString::fromUtf8(ri.method);
        const QString url = ri.url.toString();
        const QHash<QString, QString> headers = ri.headers;
        const QString firstPartyUrl = ri.firstPartyUrl.toString();
        const int resourceType = ri.resourceType;
        QMetaObject::invokeMethod(self,
            [self, method, url, headers, firstPartyUrl, resourceType]() {
                self->onInterceptorRequest(method, url, headers,
                                           firstPartyUrl, resourceType);
            }, Qt::QueuedConnection);
    });
}

// HTTP GET /json 同步拉取 target 列表（跑在 GUI 线程，短暂同步可接受——单次 localhost HTTP）。
// 用 Qt 的 QNetworkAccessManager 同步包装（避免 httplib 的 IPv6/DNS 解析问题，
// Qt 网络栈更可靠地走 127.0.0.1）。
QString HarRecorder::discoverTargetId(const QString& pageUrl) {
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QStringLiteral("http://%1:%2/json")
                                 .arg(m_debugHost).arg(m_debugPort)));
    QNetworkReply* reply = nam.get(req);
    // 阻塞等待（localhost，快）。用 QEventLoop 避免递归事件循环。
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // 3s 超时兜底。
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return {};
    }
    QByteArray body = reply->readAll();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return {};
    }

    // URL 规范化比较：Chromium 会给 example.com 补尾部斜杠，直接字符串匹配会漏。
    // 用 QUrl 规范化后比较（scheme + host + path，忽略 fragment/query 差异）。
    const QUrl want(pageUrl);
    const QString wantHost = want.host().toLower();

    // 并发同 URL 时取最后一个（最新创建的 page target）。
    // 匹配策略：page 刚创建时 URL 可能是空串/about:blank（load 前）或已加载目标 URL。
    // 三级回退：1) 精确 URL 匹配 2) about:blank 3) 最后一个 page target（最新创建的，load 前最可能是它）。
    // 并发安全：跳过已被其他 session 占用的 target（m_usedTargetIds），避免多 task 连同一 page。
    QString best;
    QString aboutBlankId;
    QString lastPageId;
    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        if (o.value("type").toString() != QStringLiteral("page")) continue;
        const QString id = o.value("id").toString();
        // 跳过已被其他 session 占用的 target（并发隔离）
        if (m_usedTargetIds.contains(id)) continue;
        const QString targetUrlStr = o.value("url").toString();
        const QUrl targetUrl(targetUrlStr);
        lastPageId = id;  // 记最后一个未占用的 page（JSON 数组顺序≈创建顺序）
        // 精确 URL 匹配（host + path 前缀）
        if (!wantHost.isEmpty() && targetUrl.host().toLower() == wantHost) {
            const QString wantPath = want.path();
            const QString gotPath = targetUrl.path();
            if (gotPath.startsWith(wantPath) || wantPath.startsWith(gotPath)
                || (wantPath.isEmpty() && gotPath == QStringLiteral("/"))) {
                best = id;
            }
        }
        // 回退：记录 about:blank（load 前的 page）
        if (targetUrlStr == QStringLiteral("about:blank")) {
            aboutBlankId = id;
        }
    }
    if (!best.isEmpty()) return best;
    if (!aboutBlankId.isEmpty()) return aboutBlankId;
    return lastPageId;  // 最后回退：最新创建的未占用 page target
}

void HarRecorder::attachForTask(const QString& taskId, const QString& pageUrl) {
    if (!isEnabled()) return;
    if (m_sessions.contains(taskId)) return;  // 已 attach（retry 路径可能重复调）

    auto session = std::make_shared<Session>();
    session->pageUrl = pageUrl;
    session->client = std::make_unique<CdpClient>(this);

    setupEventHandlers(taskId, *session);

    // 先建 session 占位，异步发现 targetId 后再连接。
    m_sessions.insert(taskId, session);

    // 发现 targetId 并立即同步连接。
    // page 在 startAttempt 里刚创建（view = new QWebEngineView），但 CDP /json 注册有延迟。
    // 用紧凑的同步轮询（最多 ~500ms，每 10ms 一次）等 target 出现，找到后立即同步连接 WebSocket
    // + 等 Network.enable 完成，然后才返回——确保后续 load() 的请求能被 CDP 捕获。
    // 这个阻塞跑在 GUI 线程但很短（localhost，~50-200ms），可接受（与 settle/采集的异步等待同级）。
    QString targetId;
    for (int i = 0; i < 50; ++i) {  // 50 × 10ms = 500ms 上限
        targetId = discoverTargetId(pageUrl);
        if (!targetId.isEmpty()) break;
        // 短暂让出事件循环，让 Chromium 注册 page target。
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (targetId.isEmpty()) {
        qWarning("[har] task=%s attach failed: targetId not found for url=%s (poll timeout)",
                 taskId.toUtf8().constData(), pageUrl.toUtf8().constData());
        return;
    }
    qWarning("[har] task=%s attaching to target=%s", taskId.toUtf8().constData(),
             targetId.toUtf8().constData());
    // 锁定 targetId（并发隔离：其他 session 发现时会跳过这个 target）。
    m_usedTargetIds.insert(targetId);
    session->targetId = targetId;
    session->client->connectToTarget(m_debugHost, m_debugPort, targetId, /*waitForReady=*/true);
}

void HarRecorder::retryDiscover(const QString& taskId, const QString& pageUrl, int attempt) {
    auto it = m_sessions.find(taskId);
    if (it == m_sessions.end()) return;  // 已被 detach（任务快速结束）
    Session& s = *it.value();
    if (s.finalized) return;
    if (s.client->isConnected()) return;  // 已连上（重试路径防重复）

    const QString targetId = discoverTargetId(pageUrl);
    // 首次同步发现失败时（page target 尚未注册到 /json），立即延后重试。
    // 不阻塞 startAttempt 调用链——首次同步查不到就走异步重试。
    if (!targetId.isEmpty()) {
        s.client->connectToTarget(m_debugHost, m_debugPort, targetId);
        qWarning("[har] task=%s attaching to target=%s (attempt %d)",
                 taskId.toUtf8().constData(), targetId.toUtf8().constData(), attempt);
        return;
    }
    // 重试：50ms / 100ms / 200ms / 400ms（指数退避），最多 5 次。
    if (attempt < 5) {
        const int delay = 50 * (1 << (attempt - 1));  // attempt 1->50, 2->100, ...
        QTimer::singleShot(delay, this, [this, taskId, pageUrl, attempt]() {
            retryDiscover(taskId, pageUrl, attempt + 1);
        });
    } else {
        qWarning("[har] task=%s attach failed: targetId not found for url=%s after %d attempts",
                 taskId.toUtf8().constData(), pageUrl.toUtf8().constData(), attempt);
    }
}

void HarRecorder::setupEventHandlers(const QString& taskId, Session& s) {
    CdpClient* client = s.client.get();

    client->on(QStringLiteral("Network.requestWillBeSent"),
        [this, taskId](const QJsonObject& params) {
            auto sit = m_sessions.find(taskId);
            if (sit == m_sessions.end()) return;
            Session& s = *sit.value();

            const QString reqId = params.value("requestId").toString();
            const QJsonObject req = params.value("request").toObject();

            // 已有 entry（重定向会产生新 requestWillBeSent 带 redirectResponse）：
            // 先把上一个 response 定稿归档（通过 redirectResponse）。
            const bool hasRedirect = params.contains("redirectResponse");
            if (hasRedirect && s.entries.contains(reqId)) {
                // 重定向：用 redirectResponse 补上一个阶段的 response，entry 定稿。
                HarEntryBuilder* b = s.entries.value(reqId);
                const QJsonObject rr = params.value("redirectResponse").toObject();
                b->hasResponse = true;
                b->status = rr.value("status").toInt();
                b->statusText = rr.value("statusText").toString();
                b->httpVersionResp = normalizeHttpVersion(rr.value("protocol").toString());
                b->mimeType = rr.value("mimeType").toString();
                b->remoteIp = rr.value("remoteIPAddress").toString();
                b->connectionId = rr.value("connectionId").toInt();
                b->respTimestamp = params.value("timestamp").toDouble();
                b->finished = true;
                b->finishedTimestamp = params.value("timestamp").toDouble();
                // 新请求改用新 requestId 继续跟踪。
            }

            HarEntryBuilder* b = new HarEntryBuilder();
            b->requestId = reqId;
            b->method = req.value("method").toString();
            b->url = req.value("url").toString();
            b->httpVersionReq = normalizeHttpVersion(req.value("protocol").toString());
            const QJsonObject headers = req.value("headers").toObject();
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                b->reqHeaders.insert(it.key(), it.value().toString());
            }
            if (req.value("hasPostData").toBool()) {
                b->hasPostData = true;
                b->postData = req.value("postData").toString();
                b->postMime = req.value("headers").toObject()
                                  .value("Content-Type").toString();
            }
            b->wallTime = params.value("wallTime").toDouble();
            b->cdpTimestamp = params.value("timestamp").toDouble();
            b->resourceType = resourceTypeLower(params.value("type").toString());
            b->initiator = params.value("initiator").toObject();

            s.entries.insert(reqId, b);
        });

    client->on(QStringLiteral("Network.requestWillBeSentExtraInfo"),
        [this, taskId](const QJsonObject& params) {
            auto sit = m_sessions.find(taskId);
            if (sit == m_sessions.end()) return;
            Session& s = *sit.value();
            const QString reqId = params.value("requestId").toString();
            HarEntryBuilder* b = s.entries.value(reqId);
            if (!b) return;
            // 合并真实线上请求头（extra info 优先，含 Cookie）。
            const QJsonObject headers = params.value("headers").toObject();
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                b->reqHeaders.insert(it.key(), it.value().toString());
            }
        });

    client->on(QStringLiteral("Network.responseReceived"),
        [this, taskId](const QJsonObject& params) {
            auto sit = m_sessions.find(taskId);
            if (sit == m_sessions.end()) return;
            Session& s = *sit.value();
            const QString reqId = params.value("requestId").toString();
            HarEntryBuilder* b = s.entries.value(reqId);
            if (!b) return;

            const QJsonObject resp = params.value("response").toObject();
            b->hasResponse = true;
            b->status = resp.value("status").toInt();
            b->statusText = resp.value("statusText").toString();
            b->httpVersionResp = normalizeHttpVersion(resp.value("protocol").toString());
            b->mimeType = resp.value("mimeType").toString();
            b->remoteIp = resp.value("remoteIPAddress").toString();
            b->connectionId = resp.value("connectionId").toInt();
            b->timing = resp.value("timing").toObject();
            b->respTimestamp = params.value("timestamp").toDouble();
            // 资源类型：若 requestWillBeSent 没给，用 responseReceived 的补。
            if (b->resourceType.isEmpty()) {
                b->resourceType = resourceTypeLower(params.value("type").toString());
            }
            const QJsonObject headers = resp.value("headers").toObject();
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                b->respHeaders.insert(it.key(), it.value().toString());
            }
        });

    client->on(QStringLiteral("Network.responseReceivedExtraInfo"),
        [this, taskId](const QJsonObject& params) {
            auto sit = m_sessions.find(taskId);
            if (sit == m_sessions.end()) return;
            Session& s = *sit.value();
            const QString reqId = params.value("requestId").toString();
            HarEntryBuilder* b = s.entries.value(reqId);
            if (!b) return;
            // 合并真实响应头（含 Set-Cookie，base event 常被剥）。
            const QJsonObject headers = params.value("headers").toObject();
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                b->respHeaders.insert(it.key(), it.value().toString());
            }
        });

    client->on(QStringLiteral("Network.loadingFinished"),
        [this, taskId](const QJsonObject& params) {
            auto sit = m_sessions.find(taskId);
            if (sit == m_sessions.end()) return;
            Session& s = *sit.value();
            const QString reqId = params.value("requestId").toString();
            HarEntryBuilder* b = s.entries.value(reqId);
            if (!b) return;

            b->finished = true;
            b->finishedTimestamp = params.value("timestamp").toDouble();
            b->encodedDataLength = static_cast<qint64>(params.value("encodedDataLength").toDouble(-1));

            // 立即取响应体（必须在 loadingFinished 后才能取；page 销毁后取不到）。
            // 大响应（>1MB）降级：只留 size 不留 text，省内存。
            s.client->send(QStringLiteral("Network.getResponseBody"),
                [&]() -> QJsonObject {
                    QJsonObject p;
                    p["requestId"] = reqId;
                    return p;
                }(),
                [this, taskId, reqId](const QJsonObject& result, bool ok) {
                    auto sit = m_sessions.find(taskId);
                    if (sit == m_sessions.end()) return;
                    Session& s = *sit.value();
                    HarEntryBuilder* b = s.entries.value(reqId);
                    if (!b) return;
                    b->bodyFetched = true;
                    if (ok) {
                        b->body = result.value("body").toString();
                        b->bodyIsBase64 = result.value("base64Encoded").toBool();
                        b->decodedBodySize = static_cast<qint64>(b->body.toUtf8().size());
                        // 大响应降级：>1MB 不留 text（省内存，仍保留 size/mimeType）。
                        if (b->decodedBodySize > 1024 * 1024) {
                            b->body.clear();
                        }
                    }
                });
        });

    client->on(QStringLiteral("Network.loadingFailed"),
        [this, taskId](const QJsonObject& params) {
            auto sit = m_sessions.find(taskId);
            if (sit == m_sessions.end()) return;
            Session& s = *sit.value();
            const QString reqId = params.value("requestId").toString();
            HarEntryBuilder* b = s.entries.value(reqId);
            if (!b) return;
            b->finished = true;
            b->failed = true;
            b->errorText = params.value("errorText").toString();
            b->finishedTimestamp = params.value("timestamp").toDouble();
        });
}

void HarRecorder::detachForTask(const QString& taskId) {
    // 给在途的 getResponseBody 一点时间回来（异步）。0.3s 足够 localhost CDP 往返。
    // 不阻塞调用方（destroyPage）——用 singleShot 延后 finalize。
    QTimer::singleShot(300, this, [this, taskId]() {
        finalizeAndArchive(taskId);
    });
}

void HarRecorder::finalizeAndArchive(const QString& taskId) {
    auto it = m_sessions.find(taskId);
    if (it == m_sessions.end()) return;
    std::shared_ptr<Session> session = it.value();
    m_sessions.erase(it);
    if (session->finalized) return;
    session->finalized = true;

    // 释放 targetId 占用（允许后续新任务复用该 target 槽位）。
    if (!session->targetId.isEmpty()) {
        m_usedTargetIds.remove(session->targetId);
    }

    // 断开 CDP（target 随 page 销毁自动消失，无需 closeTarget）。
    session->client->close();

    // 聚合所有 entry 为 HAR JSON。
    QJsonArray entriesArr;
    for (auto eit = session->entries.begin(); eit != session->entries.end(); ++eit) {
        HarEntryBuilder* b = eit.value();
        if (b->hasResponse || b->finished) {
            entriesArr.append(b->buildHarEntry());
        }
        delete b;
    }
    session->entries.clear();

    QJsonObject log;
    log["version"] = QStringLiteral("1.2");
    QJsonObject creator;
    creator["name"] = QStringLiteral("seimi-render");
    creator["version"] = QStringLiteral(SEIMI_VERSION);
    log["creator"] = creator;
    log["entries"] = entriesArr;

    QJsonObject root;
    root["log"] = log;

    const QString harJson = QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));

    if (m_store) {
        m_store->archive(taskId, harJson);
    }
    qWarning("[har] task=%s archived %d entries", taskId.toUtf8().constData(),
             static_cast<int>(entriesArr.size()));
}

void HarRecorder::onInterceptorRequest(const QString& method,
                                       const QString& url,
                                       const QHash<QString, QString>& headers,
                                       const QString& firstPartyUrl, int resourceType) {
    // 兜底：CDP 偶发漏 requestWillBeSent 时，用拦截器数据补全。
    // 注意：拦截器无法精确关联到 taskId（它只看到请求，不知道是哪个 page 发的）。
    // 这里按 URL 匹配当前所有活跃 session 的 entry，找到就补全 reqHeaders/method。
    for (auto sit = m_sessions.begin(); sit != m_sessions.end(); ++sit) {
        Session& s = *sit.value();
        for (auto eit = s.entries.begin(); eit != s.entries.end(); ++eit) {
            HarEntryBuilder* b = eit.value();
            if (b->url == url && b->method.isEmpty()) {
                b->method = method;
                for (auto hit = headers.begin(); hit != headers.end(); ++hit) {
                    if (!b->reqHeaders.contains(hit.key())) {
                        b->reqHeaders.insert(hit.key(), hit.value());
                    }
                }
                b->interceptedReqSeen = true;
            }
        }
    }
}

} // namespace seimi
