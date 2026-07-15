// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "RenderPool.h"

#include "CookieStore.h"
#include "ProxyConfig.h"
#include "SsrfInterceptor.h"
#include "StealthProfile.h"

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QPainter>
#include <QProcess>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QStringList>
#include <QStringView>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>

#include <cstdio>

#include <html2md.h>

namespace seimi {

static qint64 nowMsec() { return QDateTime::currentMSecsSinceEpoch(); }

// 把不可信字符串净化为单行日志安全形式：C0 控制符（含 CR/LF）替换为空格，防日志伪造
//（攻击者在 URL 里塞 \r\n 制造假日志行，规避 SIEM / 误导运维排查）。仅用于日志输出，
// 不影响回传给客户端的内容（客户端内容走 JSON esc）。
static QString sanitizeForLog(const QString& s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        out.append((c.unicode() < 0x20) ? QChar(QChar::Space) : c);
    }
    return out;
}

// Readability 包装器（extract.js）的静态缓存，首次 extractReadability 调用时从磁盘加载。
QString RenderWorker::s_readabilityJs;
// DOM 简化器（simplify.js）的静态缓存，首次 extractSimplifiedHtml 调用时从磁盘加载。
QString RenderWorker::s_simplifyJs;
QHash<QString, QString> RenderWorker::s_serpJsCache;
// 视口是否用 stealth 统一尺寸（由 RenderPool::start 设置一次，所有 worker 共享）。
bool RenderWorker::s_stealthViewport = false;

// ============================================================
// RenderWorker
// ============================================================

RenderWorker::RenderWorker(QWebEngineProfile* profile, RenderQueue* queue,
                           qint64 loadTimeoutMsec, QObject* parent)
    : QObject(parent)
    , m_profile(profile)
    , m_queue(queue)
    , m_loadTimeoutMsec(loadTimeoutMsec)
    , m_view(nullptr)
    , m_page(nullptr)
    , m_busy(false)
    , m_collectStarted(false) {
    // view/page 在 assign 时按需创建，避免空闲时占用资源。

    m_loadTimer.setSingleShot(true);
    m_settleTimer.setSingleShot(true);

    connect(&m_loadTimer, &QTimer::timeout, this, &RenderWorker::onLoadTimeout);
    connect(&m_settleTimer, &QTimer::timeout, this, &RenderWorker::onSettleElapsed);
}

RenderWorker::~RenderWorker() {
    destroyPage();
}

void RenderWorker::assign(RenderTaskPtr task) {
    // 仅在 GUI 线程调用；此时 worker 必然空闲（由 RenderPool::dispatch 保证）。
    Q_ASSERT(m_view == nullptr && m_page == nullptr);
    m_task = std::move(task);
    m_busy = true;
    m_collectStarted = false;
    m_pendingCollect = 0;
    m_task->startedAtMsec = nowMsec();

    // 每任务一个全新 view（持有 page）。用 view 而非裸 page 是为了 QWidget::grab() 截图
    //（QWebEnginePage 在 Qt6 无 grabToImage）。
    m_view = new QWebEngineView(m_profile, nullptr);
    // 视口尺寸：stealth 开启时统一 1920×1080（与 screen 指纹一致），否则 1280×2000。
    if (s_stealthViewport) {
        m_view->resize(StealthProfile::viewportWidth(), StealthProfile::viewportHeight());
        // stealth 背景色：引擎层直接设白，先于任何 JS 执行，挡住 creepjs 检测。
        m_view->page()->setBackgroundColor(Qt::white);
    } else {
        m_view->resize(1280, 2000);
    }
    // offscreen 下激活渲染管线：必须 show() 才能让 Blink 把像素合成进 backing store，
    // 否则 grab() 拿空白。WA_DontShowOnSpeech 确保 show() 不弹真实窗口。
    m_view->setAttribute(Qt::WA_DontShowOnScreen);
    m_view->show();
    m_page = m_view->page();
    connect(m_page, &QWebEnginePage::loadFinished, this, &RenderWorker::onLoadFinished);
    connect(m_view, &QWebEngineView::destroyed, this, [this](QObject*) {
        m_view = nullptr;
        m_page = nullptr;
    });

    // 重置预取降级状态（每任务独立）。
    m_prefetchedHtml.clear();
    m_effectivePrefetchUrl.clear();
    m_prefetchDone = false;
    m_prefetchUsed = false;

    m_loadTimer.start(int(m_loadTimeoutMsec));

    // 搜索引擎 URL：并行启动预取绕 TLS 风控（详见 startPrefetch）。
    const QString targetUrl = m_task->url;
    if (isSearchEngineUrl(targetUrl)) {
        startPrefetch(targetUrl);
    }

    m_page->load(QUrl(targetUrl));
}

void RenderWorker::onLoadFinished(bool ok) {
    if (!m_busy || !m_task || !m_page) return;

    // Chromium 网络错误页检测：目标不可达时 Chromium 加载 chrome-error:// 内置错误页，
    // 该页 loadFinished(ok=true) 且 body 远超 512 字节，不拦截会被误判为渲染成功，
    // agent 拿错误页 markdown 当真实内容。
    const QUrl curUrl = m_page->url();
    const bool isChromeErrorPage =
        curUrl.scheme() == QLatin1String("chrome-error")
        || curUrl.scheme() == QLatin1String("chrome-network-error");
    if (isChromeErrorPage) {
        // TLS 风控降级：Chromium 被加载错误页，但若预取成功，用预取 HTML 经 setHtml 重渲染。
        if (tryPrefetchFallback()) return;
        finishFailure(QStringLiteral(
            "page load failed: target unreachable (Chromium network error page). "
            "Likely DNS/connection/SSL failure. url=%1")
            .arg(curUrl.toString()));
        return;
    }

    if (!ok) {
        // loadFinished(false) 触发条件宽泛：主文档失败、任意子资源失败、JS challenge 中间态
        // 都会触发。后两者主文档其实已就绪。先探测 DOM：body >= 512 字节（远大于真错误页如
        // Google 404 的 ~260 字节）视为可采集，继续 settle；否则判失败。
        m_page->runJavaScript(
            QStringLiteral("(document.body&&document.body.innerHTML.length)||0"),
            [this](const QVariant& v) {
                if (!m_busy || !m_task || !m_page) return;
                int bodyLen = v.toInt();
                static constexpr int kMinViableBodyLen = 512;
                if (bodyLen >= kMinViableBodyLen) {
                    m_settleTimer.start(m_task->settleDelayMsec);
                } else {
                    if (tryPrefetchFallback()) return;
                    finishFailure(QStringLiteral(
                        "page load finished with error (http 4xx/5xx or network); "
                        "body too small (%1 bytes)").arg(bodyLen));
                }
            });
        return;
    }
    m_settleTimer.start(m_task->settleDelayMsec);
}

void RenderWorker::onLoadTimeout() {
    if (!m_busy || !m_task) return;
    // 搜索引擎 TLS 风控降级：超时可能是 Chromium 反复重试被 RST 的连接。
    // 若预取已完成且有结果，用 setHtml 重渲染（预取用 OpenSSL 不被风控）。
    if (tryPrefetchFallback()) return;
    finishFailure(QStringLiteral("render timed out after %1 ms").arg(m_loadTimeoutMsec));
}

// —— 搜索引擎 TLS 风控降级 ——
// Google/Bing 基于 TLS 指纹(JA3/JA4) 在握手层 RST Chromium 的 BoringSSL 连接，
// 而 curl(系统 OpenSSL) 不被识别能正常预取。搜索引擎结果页高度 JS 化，curl 预取后
// 直接 html2md 转不出内容，故需 setHtml 让 Chromium 执行 JS。

bool RenderWorker::isSearchEngineUrl(const QString& url) {
    // 仅对主流搜索引擎结果页启用降级（精准匹配，避免误伤普通页面）。
    static const QRegularExpression re(
        QStringLiteral("https?://(?:www\\.)?(?:google|bing|duckduckgo)\\.[^/]+/(?:search|webhp)\\?"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(url).hasMatch();
}

void RenderWorker::startPrefetch(const QString& url) {
    // 用系统 curl 预取：curl 的 TLS 栈与 Chromium BoringSSL 指纹不同，能绕过 Google TLS 风控；
    // QNAM 在某些 Qt 构建下首请求报 err=99 TLS initialization failed，curl 最可靠。
    auto* proc = new QProcess(this);
    // 走应用代理（与 Chromium 一致）、伪装 Chrome UA、跟随重定向。
    // -w "\n%{url_effective}"：响应体末尾追加最终 URL，供 setHtml 降级前的同源校验。
    QStringList args = {
        QStringLiteral("-s"), QStringLiteral("--max-time"), QStringLiteral("12"),
        QStringLiteral("-L"),  // 跟随重定向
        QStringLiteral("-A"), QString::fromLatin1(StealthProfile::userAgent()),
        QStringLiteral("-H"), QStringLiteral("Accept-Language: en-US,en;q=0.9"),
        QStringLiteral("-w"), QStringLiteral("\n%{url_effective}"),
        url
    };
    // 走应用代理（与 Chromium 一致）。
    QNetworkProxy appProxy = QNetworkProxy::applicationProxy();
    if (appProxy.type() == QNetworkProxy::HttpProxy || appProxy.type() == QNetworkProxy::Socks5Proxy) {
        QString proxyUrl = appProxy.type() == QNetworkProxy::Socks5Proxy
            ? QStringLiteral("socks5h://%1:%2").arg(appProxy.hostName()).arg(appProxy.port())
            : QStringLiteral("http://%1:%2").arg(appProxy.hostName()).arg(appProxy.port());
        args.prepend(QStringLiteral("-x"));
        args.insert(1, proxyUrl);
    }
    qWarning("[prefetch] curl start: %s", sanitizeForLog(url).left(60).toUtf8().constData());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
        proc->deleteLater();
        if (!m_busy || !m_task) return;
        if (code != 0) {
            m_prefetchDone = true;
            qWarning("[prefetch] curl failed: exit=%d", code);
            return;
        }
        QByteArray out = proc->readAll();
        // curl 末尾以 -w 追加了最终 URL（无换行），故最后一个换行之后是 effective URL，之前是响应体。
        const int lastNl = out.lastIndexOf('\n');
        if (lastNl >= 0) {
            m_effectivePrefetchUrl = QString::fromUtf8(out.mid(lastNl + 1)).trimmed();
            out = out.left(lastNl);
        }
        // 体积上限：异常/攻击性超大响应丢弃。
        if (out.size() > kMaxPrefetchBytes) {
            m_prefetchedHtml.clear();
            m_effectivePrefetchUrl.clear();
            m_prefetchDone = true;
            qWarning("[prefetch] curl output too large (%lld bytes), discarding",
                     static_cast<long long>(out.size()));
        } else {
            m_prefetchedHtml = QString::fromUtf8(out);
            m_prefetchDone = true;
            qWarning("[prefetch] curl success: %lld chars, effective=%s",
                     static_cast<long long>(m_prefetchedHtml.size()),
                     sanitizeForLog(m_effectivePrefetchUrl).left(80).toUtf8().constData());
        }
        // 预取晚于 Chromium 失败完成的兜底：若任务仍在进行且未用过降级，主动触发。
        if (m_busy && !m_prefetchUsed && !m_prefetchedHtml.isEmpty()) {
            QMetaObject::invokeMethod(this, &RenderWorker::tryPrefetchFallback, Qt::QueuedConnection);
        }
    });
    proc->start(QStringLiteral("curl"), args);
}

bool RenderWorker::tryPrefetchFallback() {
    if (m_prefetchUsed || !m_prefetchDone || m_prefetchedHtml.isEmpty() || !m_page) {
        qWarning("[prefetch-fallback] skip: used=%d done=%d htmlEmpty=%d page=%d",
                 m_prefetchUsed, m_prefetchDone, m_prefetchedHtml.isEmpty(), m_page != nullptr);
        return false;
    }
    m_prefetchUsed = true;
    // 同源校验：curl -L 可能跟随重定向到别的域，而 setHtml 第二参以原始 URL 为安全 origin，
    // 这会让跨站 HTML 继承目标域的 cookie scope 执行。落点 host 与请求 host 不同则拒绝降级。
    if (!m_effectivePrefetchUrl.isEmpty()) {
        const QUrl req(m_task->url);
        const QUrl eff(m_effectivePrefetchUrl);
        if (!req.host().isEmpty() && !eff.host().isEmpty()
            && req.host().compare(eff.host(), Qt::CaseInsensitive) != 0) {
            qWarning("[prefetch-fallback] skip cross-origin redirect: effective host %s != requested %s",
                     eff.host().toUtf8().constData(), req.host().toUtf8().constData());
            return false;
        }
    }
    qWarning("[prefetch-fallback] ACTIVATED: setHtml %lld chars", static_cast<long long>(m_prefetchedHtml.size()));
    m_page->setHtml(m_prefetchedHtml, QUrl(m_task->url));
    m_loadTimer.start(int(m_loadTimeoutMsec));
    return true;
}

void RenderWorker::onPrefetched(const QString& html) {
    // 保留接口（未来可用于预取完成的额外处理），当前 startPrefetch 的 lambda 已处理。
    Q_UNUSED(html);
}

void RenderWorker::onSettleElapsed() {
    if (!m_busy || !m_task || !m_page) return;
    if (m_collectStarted) return;
    // 激活懒加载图片：很多新闻/SPA 站用 lazy-loading，图片只在滚动进视口时加载。
    // 注入 JS 把常见 lazy 属性拷回 src 并滚动触发，回调里再 collect。
    activateLazyImagesAndScroll();
}

// 激活懒加载：把 data-src 等拷回 src，滚动到底再回顶触发视口懒加载。
void RenderWorker::activateLazyImagesAndScroll() {
    if (!m_busy || !m_page) { collect(); return; }
    static const QString js = QStringLiteral(R"(
        (function(){
          // 1) 常见 lazy 属性 -> src（无条件拷贝：data-src 值可能是 //协议相对 或 https 绝对）
          var attrs = ['data-src','data-original','data-lazy-src','data-original-src','data-img'];
          var imgs = document.querySelectorAll('img');
          var n = 0;
          imgs.forEach(function(img){
            for (var i=0;i<attrs.length;i++){
              var v = img.getAttribute(attrs[i]);
              if (v) { img.src = v; n++; break; }
            }
          });
          // 2) 滚动到底再回顶，触发滚动监听型懒加载（picture/source、IntersectionObserver）
          var h = document.body ? document.body.scrollHeight : 0;
          window.scrollTo(0, h);
          setTimeout(function(){ window.scrollTo(0, 0); }, 50);
          return n;
        })()
    )");
    m_page->runJavaScript(js, [this](const QVariant&) {
        if (!m_busy || !m_task || !m_page) { return; }
        // 被激活的图片要发网络请求，给 2500ms 让请求发出再采集。markdown 按算法分流：
        // Readability 走正文提取，Conservative 走 DOM 简化，其余直接采集。
        QTimer::singleShot(2500, this, [this]() {
            if (!m_busy || !m_task || !m_page) return;
            if (m_task->extractAlgorithm != ExtractAlgorithm::None) {
                extractSerp();              // SERP 结构化提取（与 markdown 互斥）
            } else if (m_task->wants(Output::Markdown)) {
                if (m_task->mdAlgorithm == MdAlgorithm::Readability) {
                    extractReadability();        // 正文定位（概率性，质量高）
                } else {
                    extractSimplifiedHtml();     // conservative 新实现：DOM 简化（默认）
                }
            } else {
                collect();
            }
        });
    });
}

// Readability 正文提取：注入 extract.js 抽正文 HTML 存 m_readabilityHtml，完成调 collect()。
// extract.js 首次从 third_party/readability/extract.js 加载并缓存（二进制同级优先，源码目录回退）。
// 找不到或 JS 失败时 m_readabilityHtml 留空，tryFinishCollect 回退保守策略。
void RenderWorker::extractReadability() {
    if (!m_busy || !m_task || !m_page) { collect(); return; }

    // 首次使用：加载 extract.js（二进制同级优先，源码目录回退）。
    if (s_readabilityJs.isEmpty()) {
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/readability/extract.js"),
#ifdef SEIMI_ADMIN_UI_SRC_DIR
            QStringLiteral(SEIMI_ADMIN_UI_SRC_DIR) + QStringLiteral("/../third_party/readability/extract.js"),
#endif
        };
        for (const QString& path : candidates) {
            // QFileInfo::isFile 确保是文件而非目录（防路径被解析成目录导致 open 异常）。
            QFileInfo fi(path);
            if (!fi.isFile()) continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                s_readabilityJs = QString::fromUtf8(f.readAll());
                break;
            }
        }
        if (s_readabilityJs.isEmpty()) {
            // extract.js 找不到：回退保守策略（静默降级）。
            collect();
            return;
        }
    }

    // 注入 extract.js（IIFE）。结果分块存 window.__seimiReadability，返回值仅状态 JSON（不截断）。
    // C++ 逐块 runJavaScript 读取拼接。
    m_page->runJavaScript(s_readabilityJs, [this](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        const QString s = v.toString();
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (!doc.isObject()) { collect(); return; }
        QJsonObject o = doc.object();
        if (!o.value(QStringLiteral("ok")).toBool()) {
            // 非文章页/异常：m_readabilityHtml 留空，回退保守策略。
            collect();
            return;
        }
        int totalChunks = o.value(QStringLiteral("totalChunks")).toInt(0);
        if (totalChunks <= 0) { collect(); return; }
        // 分块读取 window.__seimiReadability.chunks[i]，逐块拼接。
        readReadabilityChunks(0, totalChunks, QString());
    });
}

// 分块读取 Readability 结果（规避 runJavaScript 大返回值截断）。
void RenderWorker::readReadabilityChunks(int idx, int total, const QString& acc) {
    if (!m_busy || !m_task || !m_page) { collect(); return; }
    if (idx >= total) {
        m_readabilityHtml = acc;
        collect();
        return;
    }
    // null 防御：页面重载致 window 变量丢失则回退保守策略。
    QString js = QStringLiteral(
        "(window.__seimiReadability && window.__seimiReadability.chunks ? "
        "window.__seimiReadability.chunks[%1] : null)").arg(idx);
    m_page->runJavaScript(js, [this, idx, total, acc](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        if (v.isNull()) { collect(); return; }  // window 变量丢失，回退
        readReadabilityChunks(idx + 1, total, acc + v.toString());
    });
}

// conservative 路径：在 document 深拷贝上删 script/style/template/iframe/link 等噪音，
// 简化后 HTML 存 m_simplifiedHtml 后调 collect()。simplify.js 用 cloneNode 不动 live DOM，
// 截图/html 输出不受影响。加载/JS 失败则留空，回退 m_collectedHtml。
void RenderWorker::extractSimplifiedHtml() {
    if (!m_busy || !m_task || !m_page) { collect(); return; }

    // 首次使用：加载 simplify.js。
    if (s_simplifyJs.isEmpty()) {
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/readability/simplify.js"),
#ifdef SEIMI_ADMIN_UI_SRC_DIR
            QStringLiteral(SEIMI_ADMIN_UI_SRC_DIR) + QStringLiteral("/../third_party/readability/simplify.js"),
#endif
        };
        for (const QString& path : candidates) {
            QFileInfo fi(path);
            if (!fi.isFile()) continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                s_simplifyJs = QString::fromUtf8(f.readAll());
                break;
            }
        }
        if (s_simplifyJs.isEmpty()) {
            // simplify.js 找不到：回退原始 DOM（等价旧行为，静默降级）。
            collect();
            return;
        }
    }

    // 注入 simplify.js（IIFE）。结果分块存 window.__seimiSimplify，返回值仅状态 JSON。
    m_page->runJavaScript(s_simplifyJs, [this](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        const QString s = v.toString();
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (!doc.isObject()) { collect(); return; }
        QJsonObject o = doc.object();
        if (!o.value(QStringLiteral("ok")).toBool()) {
            // 简化失败：m_simplifiedHtml 留空，回退原始 DOM。
            collect();
            return;
        }
        int totalChunks = o.value(QStringLiteral("totalChunks")).toInt(0);
        if (totalChunks <= 0) { collect(); return; }
        readSimplifyChunks(0, totalChunks, QString());
    });
}

// 分块读取 simplify 结果（与 readReadabilityChunks 同协议，规避大返回值截断）。
void RenderWorker::readSimplifyChunks(int idx, int total, const QString& acc) {
    if (!m_busy || !m_task || !m_page) { collect(); return; }
    if (idx >= total) {
        m_simplifiedHtml = acc;
        collect();
        return;
    }
    QString js = QStringLiteral(
        "(window.__seimiSimplify && window.__seimiSimplify.chunks ? "
        "window.__seimiSimplify.chunks[%1] : null)").arg(idx);
    m_page->runJavaScript(js, [this, idx, total, acc](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        if (v.isNull()) { collect(); return; }  // window 变量丢失，回退
        readSimplifyChunks(idx + 1, total, acc + v.toString());
    });
}

// SERP 结构化提取：按 extractAlgorithm 选引擎 JS，注入后在 live DOM 抽取结果 JSON 存 m_serpJsonResult。
// JS 首次从 third_party/serp/<engine>.js 加载并按引擎名缓存。加载/JS 失败则留空，降级正常渲染。
void RenderWorker::extractSerp() {
    if (!m_busy || !m_task || !m_page) { collect(); return; }

    // 按 extractAlgorithm 选引擎 JS 文件名。
    const QString engine = QString::fromUtf8(extractAlgorithmName(m_task->extractAlgorithm));
    const QString jsFile = engine + QStringLiteral(".js");

    // 按引擎名查缓存（各引擎独立缓存，不互相覆盖）。
    QString& js = s_serpJsCache[engine];
    if (js.isEmpty()) {
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/serp/") + jsFile,
#ifdef SEIMI_ADMIN_UI_SRC_DIR
            QStringLiteral(SEIMI_ADMIN_UI_SRC_DIR) + QStringLiteral("/../third_party/serp/") + jsFile,
#endif
        };
        for (const QString& path : candidates) {
            QFileInfo fi(path);
            if (!fi.isFile()) continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                js = QString::fromUtf8(f.readAll());
                break;
            }
        }
        if (js.isEmpty()) {
            // 引擎 JS 找不到：直接采集（m_serpJsonResult 留空）。
            collect();
            return;
        }
    }

    // 注入引擎 JS（IIFE）。结果分块存 window.__seimiSerp，返回值仅状态 JSON。
    m_page->runJavaScript(js, [this](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        const QString s = v.toString();
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (!doc.isObject()) { collect(); return; }
        QJsonObject o = doc.object();
        if (!o.value(QStringLiteral("ok")).toBool()) {
            // JS 异常：m_serpJsonResult 留空，降级正常渲染。
            collect();
            return;
        }
        int totalChunks = o.value(QStringLiteral("totalChunks")).toInt(0);
        if (totalChunks <= 0) { collect(); return; }
        readSerpChunks(0, totalChunks, QString());
    });
}

// 分块读取 SERP 结果（与 readSimplifyChunks 同协议，规避大返回值截断）。
void RenderWorker::readSerpChunks(int idx, int total, const QString& acc) {
    if (!m_busy || !m_task || !m_page) { collect(); return; }
    if (idx >= total) {
        m_serpJsonResult = acc;
        collect();
        return;
    }
    QString js = QStringLiteral(
        "(window.__seimiSerp && window.__seimiSerp.chunks ? "
        "window.__seimiSerp.chunks[%1] : null)").arg(idx);
    m_page->runJavaScript(js, [this, idx, total, acc](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        if (v.isNull()) { collect(); return; }  // window 变量丢失，降级
        readSerpChunks(idx + 1, total, acc + v.toString());
    });
}
// settle 到期后发起采集。grab 同步、toHtml 异步，用 pendingCollect 协调：先同步截图，
// 再 toHtml，回调到达后收尾。page/view 在 toHtml 回调返回前不能销毁。
void RenderWorker::collect() {
    m_collectStarted = true;
    m_collectedHtml.clear();
    m_collectedPdf.clear();
    m_collectedImage.clear();
    // 不清 m_readabilityHtml / m_simplifiedHtml：它们在 collect 前由 extract* 准备好，
    // 在 assign/destroyPage 时复位。
    m_stitchCanvas = QImage();
    m_stitchY = 0;
    m_pendingCollect = 0;

    // 1) HTML（始终采集；markdown 转换也依赖它）。
    ++m_pendingCollect;
    m_page->toHtml([this](const QString& html) {
        if (m_busy && m_task && !m_task->done) onHtmlCollected(html);
        else decCollect();
    });

    // 2) PDF（printToPdf 是 Chromium 打印管线，offscreen 下稳定）。
    if (m_task->wants(Output::Pdf)) {
        ++m_pendingCollect;
        m_page->printToPdf([this](const QByteArray& pdf) {
            if (m_busy && m_task && !m_task->done) m_collectedPdf = pdf;
            decCollect();
        });
    }

    // 3) 截图（PNG，真实像素，所见即所得）。异步状态机（含重排/滚动等待），
    //    完成后自行 decCollect()，与 html/pdf 同等参与计数协调。
    if (m_task->wants(Output::Screenshot)) {
        ++m_pendingCollect;
        captureScreenshot();
    }
}

void RenderWorker::onHtmlCollected(const QString& html) {
    m_collectedHtml = html;
    decCollect();
}

// 检测 Chromium 内置网络错误页（ERR_xxx）。这类页 loadFinished(ok=true) 且 body 远超 512，
// 仅靠 ok/body 长度无法区分。用内容特征：含 ERR_ 错误码 + Chromium 错误页独有标记。
// 返回错误描述，非错误页返回空。
static QString detectChromeErrorPage(const QString& html) {
    if (html.isEmpty()) return {};
    // Chromium 错误页可靠特征：ERR_ + 大写错误码，且含 --error-code-color / interstitial 等独有标记。
    // ERR_ 可能藏在大段 <style> 后，故全文搜索（正则对 ~250KB 仅毫秒级）。
    static const QRegularExpression reErr(QStringLiteral("ERR_[A-Z_]{3,}"));
    QRegularExpressionMatch m = reErr.match(html);
    if (!m.hasMatch()) return {};
    QString errCode = m.captured(0);
    // 二次确认是 Chromium 错误页（而非碰巧含 ERR_ 文本的普通页面）
    bool hasChromeErrorMarker =
        html.contains(QStringLiteral("--error-code-color"))
        || html.contains(QStringLiteral("interstitial"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("took too long"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("refused to connect"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("This site"), Qt::CaseInsensitive);
    if (hasChromeErrorMarker) return errCode;
    return {};
}

// 统一的采集计数递减 + 收尾检查。
void RenderWorker::decCollect() {
    if (m_busy && m_pendingCollect > 0) --m_pendingCollect;
    tryFinishCollect();
}

// HTML/markdown/pdf 全部到齐 -> 组装结果 -> 报告成功。
void RenderWorker::tryFinishCollect() {
    if (!m_busy || !m_task) return;
    if (m_task->done) return;            // 已被超时收尾
    if (m_pendingCollect > 0) return;    // 还有采集未完成

    RenderQueue::RenderResult result;
    result.html = m_collectedHtml;

    // markdown 输入源三档优先级（html 输出始终是完整 DOM，不受算法影响）：
    //   1) Readability：算法=Readability 且抽取成功 → m_readabilityHtml（正文）
    //   2) Simplified：算法=Conservative 且简化成功 → m_simplifiedHtml
    //   3) Raw DOM 兜底：m_collectedHtml
    if (m_task->wants(Output::Markdown) && !m_collectedHtml.isEmpty()) {
        bool usedReadability = (m_task->mdAlgorithm == MdAlgorithm::Readability
                                && !m_readabilityHtml.isEmpty());
        bool usedSimplified  = (!usedReadability && !m_simplifiedHtml.isEmpty());
        const QString& mdSource = usedReadability ? m_readabilityHtml
                               : usedSimplified  ? m_simplifiedHtml
                                                 : m_collectedHtml;
        // 实际算法三态如实标注（旧代码把 raw 兜底也标 conservative，对客户端失实）。
        result.mdAlgorithmUsed = usedReadability ? MdAlgorithm::Readability
                             : usedSimplified  ? MdAlgorithm::Conservative
                                               : MdAlgorithm::Raw;
        bool ok = false;
        std::string md = html2md::Convert(mdSource.toStdString(), &ok);
        result.markdown = ok ? QString::fromStdString(md) : QString();
    }

    // SERP 结构化提取结果透传（仅 extract != None 且提取成功时）。
    if (m_task->extractAlgorithm != ExtractAlgorithm::None && !m_serpJsonResult.isEmpty()) {
        result.serpJson = m_serpJsonResult;
    }

    if (m_task->wants(Output::Pdf)) result.pdfData = m_collectedPdf;

    if (m_task->wants(Output::Screenshot)) {
        result.imageData = m_collectedImage;
        result.imageFmt = m_resolvedImageFmt;
        result.imageTruncated = m_imageTruncated;
    }

    // Chromium 网络错误页拦截（loadFinished 检测兜不住时的最后防线）：
    // 某些 Qt WebEngine 版本错误页 url 不变 chrome-error scheme，只能在拿到 html 后用内容特征检测。
    QString errCode = detectChromeErrorPage(m_collectedHtml);
    if (!errCode.isEmpty()) {
        finishFailure(QStringLiteral(
            "page load failed: target unreachable (Chromium network error page %1). "
            "Likely DNS/connection/SSL failure or proxy issue.").arg(errCode));
        return;
    }

    QString id = m_task->id;
    m_queue->reportSucceeded(id, result, nowMsec());
    destroyPage();
    emit taskFinished(id);
}

void RenderWorker::finishFailure(const QString& reason) {
    QString id = m_task->id;
    m_queue->reportFailed(id, reason, nowMsec());
    destroyPage();
    emit taskFinished(id);
}

void RenderWorker::abort() {
    // 静默化（不销毁 view/page，避免与 Chromium 异步回调重入）：停定时器、中止加载、
    // 置非忙并清任务引用。迟到的回调经守卫早退，destroyPage/析构再统一回收 view。
    m_loadTimer.stop();
    m_settleTimer.stop();
    m_busy = false;
    m_task.reset();
    if (m_view) m_view->stop();  // 中止导航/挂起中的 JS（WebEngine 安全，不重入）
}

void RenderWorker::destroyPage() {
    m_loadTimer.stop();
    m_settleTimer.stop();
    m_collectStarted = false;
    m_pendingCollect = 0;
    // 复位截图状态，避免残留影响下个任务。
    m_collectedImage.clear();
    m_stitchCanvas = QImage();
    m_stitchY = 0;
    m_resolvedImageFmt = ImageFormat::Png;
    m_imageTruncated = false;
    m_readabilityHtml.clear();
    m_simplifiedHtml.clear();
    m_serpJsonResult.clear();
    m_busy = false;          // 先标记不忙，但 m_view/m_page 保留（仍非 ready，等 destroyed 回调）
    if (m_page) {
        // 断开 loadFinished，避免旧 page 的陈旧加载信号。
        disconnect(m_page, &QWebEnginePage::loadFinished, this, &RenderWorker::onLoadFinished);
    }
    if (m_view) {
        // 异步销毁 view（其持有的 page 一并销毁）。destroyed 回调会把 m_view/m_page 置空，
        // 届时 isReady() 才变 true，dispatch 才会把新任务派来。
        m_view->deleteLater();
    }
    m_task.reset();
}

// ============================================================
// RenderWorker —— 截图（PNG，所见即所得）
// ============================================================

// 触发截图流程：先用 JS 取页面真实尺寸（宽 + 全文档高），再按高度分流。
void RenderWorker::captureScreenshot() {
    if (!m_busy || !m_task || !m_view || !m_page) { decCollect(); return; }
    // 取视口宽（clientWidth）、完整文档高度（scrollHeight），并统计 img 像素占比。
    // 后者用于 ImageFormat::Auto 的智能选择：图片密集页用 JPEG（体积小），
    // 文字页用 PNG（无损）。用 documentElement 兼容标准/怪异模式。
    static const QString js = QStringLiteral(R"(
        (function(){
          var de = document.documentElement;
          var w = de ? de.clientWidth : (window.innerWidth || 1280);
          var h = Math.max(
            document.body ? document.body.scrollHeight : 0,
            de ? de.scrollHeight : 0
          );
          // 统计所有 img 的可见渲染面积（getBoundingClientRect，含 CSS 缩放后的实际尺寸）。
          // 累加后除以页面总面积得占比。background-image 无法低成本统计，忽略。
          var imgArea = 0;
          var imgs = document.getElementsByTagName('img');
          for (var i = 0; i < imgs.length; i++) {
            var r = imgs[i].getBoundingClientRect();
            if (r.width > 0 && r.height > 0) imgArea += r.width * r.height;
          }
          var pageArea = w * h;
          var ratio = pageArea > 0 ? imgArea / pageArea : 0;
          // 先回顶，确保重置法/拼接法都从页面顶部开始采集。
          window.scrollTo(0, 0);
          return JSON.stringify({w: w, h: h, imgRatio: ratio});
        })()
    )");
    m_page->runJavaScript(js, [this](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { decCollect(); return; }
        int pw = 1280, ph = 0;
        double imgRatio = 0.0;
        const QString s = v.toString();
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (doc.isObject()) {
            QJsonObject o = doc.object();
            pw = o.value(QStringLiteral("w")).toInt(pw);
            ph = o.value(QStringLiteral("h")).toInt(ph);
            imgRatio = o.value(QStringLiteral("imgRatio")).toDouble(0.0);
        }
        if (pw < 320) pw = 320;       // 防御异常值（下限）
        if (pw > kMaxImageWidth) pw = kMaxImageWidth;  // 防御 OOM（上限）：恶意页可报超大 clientWidth
        if (ph <= 0) ph = m_view->height();  // 兜底：取当前视口高
        // 解析最终编码格式。Auto：图片像素占比 > 12% 判为照片型用 JPEG（体积小），否则 PNG。
        if (m_task->imageFormat == ImageFormat::Auto) {
            m_resolvedImageFmt = (imgRatio > 0.12) ? ImageFormat::Jpeg : ImageFormat::Png;
        } else {
            m_resolvedImageFmt = m_task->imageFormat;
        }
        onScreenshotSizeKnown(pw, ph);
    });
}

// 按页面高度分流：≤阈值走重置法（快），>阈值走滚动拼接（稳）。超 kStitchMaxHeight 截断。
void RenderWorker::onScreenshotSizeKnown(int pageWidth, int pageHeight) {
    if (!m_busy || !m_view) { decCollect(); return; }
    // 无限滚动页防护：超高度上限只截前 N 屏，避免内存/时间爆炸。
    if (pageHeight > kStitchMaxHeight) {
        m_imageTruncated = true;
        pageHeight = kStitchMaxHeight;
    }
    if (pageHeight <= kFullPageMaxPx) {
        captureByFullPageResize(pageWidth, pageHeight);
    } else {
        startScrollStitch(pageWidth, pageHeight);
    }
}

// ---- 视口重置法（短/中长页面）----
// 把 view 拉伸到全页高度，等 Blink 重排后一次性 grab。
void RenderWorker::captureByFullPageResize(int pageWidth, int pageHeight) {
    if (!m_busy || !m_view) { decCollect(); return; }
    m_view->resize(pageWidth, pageHeight);
    // resize 后 Blink 异步重排 + GPU 合成，用 QTimer 让事件循环跑一会儿再 grab（不能用 sleep，会冻结事件循环）。
    QTimer::singleShot(1000, this, [this, pageWidth, pageHeight]() {
        onFullPageResizeSettled(pageWidth, pageHeight);
    });
}

void RenderWorker::onFullPageResizeSettled(int pageWidth, int pageHeight) {
    if (!m_busy || !m_view) { decCollect(); return; }
    QPixmap pix = m_view->grab();
    if (pix.isNull()) {
        finishFailure(QStringLiteral("screenshot: view->grab() returned null (backing store not ready)"));
        return;
    }
    QImage img = pix.toImage();
    finalizeScreenshot(img);
}

// ---- 滚动拼接法（超长页面，>8000px，超 GPU 纹理上限）----
// 固定视口逐屏滚动，每屏 grab 后贴到全页画布的对应 y 偏移。
void RenderWorker::startScrollStitch(int pageWidth, int pageHeight) {
    if (!m_busy || !m_view) { decCollect(); return; }
    // 固定视口尺寸（标准屏幕高度），确保每屏像素量在 GPU 能力内。
    m_stitchViewportH = kStitchViewportH;
    m_view->resize(pageWidth, m_stitchViewportH);
    // 预分配全页画布。Format_ARGB32_Premultiplied 是 Qt 合成默认格式，grab 结果直接兼容。
    m_stitchCanvas = QImage(pageWidth, pageHeight, QImage::Format_ARGB32_Premultiplied);
    if (m_stitchCanvas.isNull()) {
        finishFailure(QStringLiteral("screenshot: failed to allocate stitch canvas (%1x%2)")
                          .arg(pageWidth).arg(pageHeight));
        return;
    }
    m_stitchY = 0;
    // 给视口尺寸变化一个重排周期，再开始第一屏。
    QTimer::singleShot(500, this, [this, pageWidth, pageHeight]() {
        captureNextScrollStitch(pageWidth, pageHeight);
    });
}

void RenderWorker::captureNextScrollStitch(int pageWidth, int pageHeight) {
    if (!m_busy || !m_view || !m_page) {
        finalizeScreenshot(m_stitchCanvas);  // 已贴部分也算结果，尽量返回
        return;
    }
    if (m_stitchY >= pageHeight) {
        // 全部屏已贴完，输出画布。
        finalizeScreenshot(m_stitchCanvas);
        return;
    }
    // 滚动到当前偏移。末屏需裁掉超出页高的部分。
    int scrollTarget = m_stitchY;
    QString js = QStringLiteral("window.scrollTo(0, %1);").arg(scrollTarget);
    m_page->runJavaScript(js, [this, pageWidth, pageHeight](const QVariant&) {
        if (!m_busy || !m_view) { finalizeScreenshot(m_stitchCanvas); return; }
        // 滚动后给懒加载/渲染一点时间再 grab。
        QTimer::singleShot(350, this, [this, pageWidth, pageHeight]() {
            if (!m_busy || !m_view) { finalizeScreenshot(m_stitchCanvas); return; }
            QPixmap pix = m_view->grab();
            if (pix.isNull()) {
                // 单屏 grab 失败不致命，跳过这屏继续（画布对应区域留空）。
                m_stitchY += m_stitchViewportH;
                captureNextScrollStitch(pageWidth, pageHeight);
                return;
            }
            QImage slice = pix.toImage();
            // 计算本屏贴到画布的高度：到页底为止（末屏裁剪）。
            int remaining = pageHeight - m_stitchY;
            int drawH = std::min({slice.height(), m_stitchViewportH, remaining});
            if (drawH > 0) {
                QPainter painter(&m_stitchCanvas);
                // 源区域：slice 顶部 drawH 像素；目标：画布 m_stitchY 处。
                painter.drawImage(0, m_stitchY, slice, 0, 0, pageWidth, drawH);
            }
            m_stitchY += m_stitchViewportH;
            captureNextScrollStitch(pageWidth, pageHeight);
        });
    });
}

// 两种路径共用：QImage → PNG/JPEG 编码 → 存入 m_collectedImage → 计数收尾。
// 编码格式由 m_resolvedImageFmt 决定（Auto 在 captureScreenshot 里已解析）。
void RenderWorker::finalizeScreenshot(const QImage& img) {
    if (!m_busy) { decCollect(); return; }
    if (img.isNull()) {
        finishFailure(QStringLiteral("screenshot: final image is null"));
        return;
    }
    // 尝试用目标格式编码；JPEG 失败（超大尺寸/内存限制/libjpeg 限制）时自动回退 PNG，
    // 而非让整个渲染任务失败——截图格式不应阻断渲染。Auto 已按占比选好，这里尊重之。
    auto tryEncode = [&](const char* fmt, int quality) -> bool {
        QImage enc = img;
        // JPEG 不支持 alpha 通道，截图画布是 ARGB32_Premultiplied，需转成 RGB888 否则会黑底。
        if (qstrncmp(fmt, "JPEG", 4) == 0 && img.hasAlphaChannel()) {
            enc = img.convertToFormat(QImage::Format_RGB888);
        }
        QBuffer buf(&m_collectedImage);
        return enc.save(&buf, fmt, quality);
    };

    const char* primaryFmt = (m_resolvedImageFmt == ImageFormat::Jpeg) ? "JPEG" : "PNG";
    int quality = (m_resolvedImageFmt == ImageFormat::Jpeg) ? 85 : -1;

    if (tryEncode(primaryFmt, quality)) {
        // 主格式编码成功。JPEG 时 m_resolvedImageFmt 已是 Jpeg；PNG 时已是 Png。
        decCollect();
        return;
    }
    // 主格式失败：JPEG 回退 PNG（无损、支持 alpha、无尺寸限制），不让截图失败阻断渲染。
    if (m_resolvedImageFmt == ImageFormat::Jpeg) {
        m_collectedImage.clear();
        if (tryEncode("PNG", -1)) {
            m_resolvedImageFmt = ImageFormat::Png;  // 更新实际格式，响应里回传正确 mime
            decCollect();
            return;
        }
    }
    // 两种都失败（极罕见，如 OOM）：报告真实错误。
    finishFailure(QStringLiteral("screenshot: %1 encode failed").arg(QLatin1String(primaryFmt)));
}

// ============================================================
// RenderPool
// ============================================================

RenderPool::RenderPool(RenderQueue* queue, int concurrency, qint64 loadTimeoutMsec, QObject* parent)
    : QObject(parent)
    , m_queue(queue)
    , m_concurrency(concurrency)
    , m_loadTimeoutMsec(loadTimeoutMsec) {}

void RenderPool::start() {
    // 共享 profile：节省内存、复用 HTTP 缓存与 cookie；OffTheRecord 关闭持久化，纯渲染场景更干净。
    m_profile = new QWebEngineProfile(QStringLiteral("seimi-render-pool"), this);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);

    // SSRF 二次防护：对 Chromium 实际发起的每个请求再判定一次，堵住重定向 /
    // DNS rebinding / 内嵌子框架等提交时校验（UrlGuard）无法覆盖的旁路。
    // stealth 开启时，interceptor 同时统一指纹相关 HTTP 头（Sec-CH-UA 等）。
    m_ssrfInterceptor = new SsrfRequestInterceptor(m_stealthEnabled, m_profile);
    m_profile->setUrlRequestInterceptor(m_ssrfInterceptor);

    // 浏览器指纹统一（stealth）：所有渲染实例伪装成同一 Chrome on Windows 10 桌面环境，
    // 绕过反爬的 webdriver/UA/特征检测。三层联动（详见 StealthProfile.h）：
    //   1) HTTP UA（profile->setHttpUserAgent）
    //   2) JS 指纹（stealth.js，DocumentCreation 注入 MainWorld，先于网页脚本覆盖 navigator/screen 等）
    //   3) HTTP 头（SsrfInterceptor 统一 Sec-CH-UA/Accept-Language 等）
    if (m_stealthEnabled) {
        RenderWorker::s_stealthViewport = true;
        m_profile->setHttpUserAgent(QString::fromLatin1(StealthProfile::userAgent()));

        // 注册 stealth.js：DocumentCreation（最早）+ MainWorld（与网页同上下文）+ runsOnSubFrames。
        // 注册一次全局生效（profile 共享）。
        QString stealthJs = StealthProfile::loadStealthJs();
        if (!stealthJs.isEmpty()) {
            QWebEngineScript script;
            script.setName(QStringLiteral("seimi-stealth"));
            script.setSourceCode(stealthJs);
            script.setInjectionPoint(QWebEngineScript::DocumentCreation);
            script.setWorldId(QWebEngineScript::MainWorld);
            script.setRunsOnSubFrames(true);
            m_profile->scripts()->insert(script);
        } else {
            std::fprintf(stderr,
                "WARNING: stealth.js not found; fingerprint unification disabled "
                "(renders continue but anti-bot detection may trigger).\n");
        }
    }

    for (int i = 0; i < m_concurrency; ++i) {
        auto w = std::make_unique<RenderWorker>(m_profile, m_queue, m_loadTimeoutMsec, this);
        connect(w.get(), &RenderWorker::taskFinished, this, &RenderPool::taskFinished);
        m_workers.push_back(std::move(w));
    }

    // 登记并发槽位总数到队列（供 stats / 管理界面表达「活跃 worker / 总数」）。
    // 槽位数是静态配置，start() 后一次性设置即可。
    m_queue->setConcurrency(m_concurrency);

    // 分发节流：高频检查空闲 worker 并派发队列任务。
    m_dispatchTimer.setInterval(20); // ms
    connect(&m_dispatchTimer, &QTimer::timeout, this, &RenderPool::dispatch);
    m_dispatchTimer.start();

    // Cookie 同步：周期把待注入 cookie apply 到共享 profile（须 GUI 线程）。500ms 足够实时。
    if (m_cookies) {
        m_cookieApplyTimer.setInterval(500);
        connect(&m_cookieApplyTimer, &QTimer::timeout, this, [this]() {
            m_cookies->applyTo(m_profile);
        });
        m_cookieApplyTimer.start();
    }

    // 代理同步：周期把待生效代理写入 setApplicationProxy（须 GUI 线程）。
    if (m_proxyConfig) {
        m_proxyApplyTimer.setInterval(500);
        connect(&m_proxyApplyTimer, &QTimer::timeout, this, [this]() {
            m_proxyConfig->apply();
        });
        m_proxyConfig->apply();  // 启动时立即 apply（让 --proxy 初始代理生效）
        m_proxyApplyTimer.start();
    }
}

void RenderPool::stop() {
    m_dispatchTimer.stop();
    m_cookieApplyTimer.stop();
    m_proxyApplyTimer.stop();
    // aboutToQuit 时事件循环仍在转，提前 abort 让在途异步回调命中早退守卫，
    // 不在析构窗口触碰半销毁状态。
    for (auto& w : m_workers) w->abort();
}

void RenderPool::dispatch() {
    // 每 tick 尽量派发多个提高吞吐。必须用非阻塞 tryTakePending：本方法在事件循环回调里，
    // 阻塞会冻结事件循环，导致 loadFinished/QTimer 全停摆。
    for (auto& w : m_workers) {
        if (w->isReady()) {  // 仅当 worker 真正空闲且 page 已销毁
            RenderTaskPtr task = m_queue->tryTakePending();
            if (!task) break; // 队列空，等下次 tick（不阻塞）
            w->assign(task);
        }
    }
}

} // namespace seimi
