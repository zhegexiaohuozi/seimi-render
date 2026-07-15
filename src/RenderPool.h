#pragma once

#include "RenderQueue.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <QImage>

#include <memory>
#include <vector>

namespace seimi {

class CookieStore;
class ProxyConfig;
class SsrfRequestInterceptor;

// 单个渲染工作单元：每任务用独立 QWebEnginePage，用完即销毁。
// 必须且只能在 GUI 线程创建/使用（WebEngine 强制）。
// 不复用 page：快速连续 load 时第二次 loadFinished 可能丢失（Chromium 竞态），
// 导致任务永久卡 running。每任务新建 page 彻底规避。
//
// 流程：
//   新建 page -> load(url) -> loadFinished(ok=true) -> settle -> toHtml -> 报告成功 -> 销毁
//   loadFinished(ok=false) -> 探测 DOM：有实质内容则继续，否则报告失败 -> 销毁
//   任意超时 -> 报告失败 -> 销毁
class RenderWorker : public QObject {
    Q_OBJECT
public:
    explicit RenderWorker(QWebEngineProfile* profile, RenderQueue* queue, qint64 loadTimeoutMsec, QObject* parent = nullptr);
    ~RenderWorker() override;

    bool isBusy() const { return m_busy; }
    // worker 可接新任务：不忙 且 page 已销毁（deleteLater 异步，旧 page 销毁前不能建新 page）。
    bool isReady() const { return !m_busy && m_page == nullptr; }
    void assign(RenderTaskPtr task);

    // 关闭期由 RenderPool::stop 调用：停定时器、中止加载、置非忙并清任务引用。
    // 使迟到的异步回调命中 if(!m_busy||!m_task||!m_page) 早退守卫。
    void abort();

signals:
    // 单任务终态完成信号（携带 task id），供主线程转发给 WebSocket 推送。
    void taskFinished(const QString& id);

private slots:
    void onLoadFinished(bool ok);
    void onLoadTimeout();       // 加载总超时（含 load + 采集阶段）
    void onSettleElapsed();     // loadFinished 后的 JS 执行延时到期

private:
    // settle 到期后，先激活懒加载图片（注入 JS + 滚动），再采集。
    void activateLazyImagesAndScroll();
    // settle 到期后发起采集（toHtml + 可选 pdf + 可选截图），全部到齐后收尾。
    void collect();
    void onHtmlCollected(const QString& html);
    void decCollect();          // 异步采集计数递减 + 收尾检查
    void tryFinishCollect();    // 检查采集是否全部到齐
    void finishFailure(const QString& reason);
    void destroyPage();         // 销毁当前 page 并复位状态

    // —— markdown Readability 正文提取（可选算法）——
    // mdAlgorithm==Readability 且 wants(Markdown) 时，注入 extract.js 抽正文 HTML 存 m_readabilityHtml。
    // tryFinishCollect 转换 markdown 时优先用它。失败（非文章页/异常）留空，回退保守策略。
    void extractReadability();
    // 分块读取结果（规避 runJavaScript 大返回值截断）。
    void readReadabilityChunks(int idx, int total, const QString& acc);

    // —— conservative 路径：DOM 简化（默认 markdown 算法）——
    // mdAlgorithm==Conservative 且 wants(Markdown) 时，注入 simplify.js 在克隆 DOM 上删噪音，
    // 简化后 HTML 存 m_simplifiedHtml。避免 html2md（字符串解析器）遇 script 内 '<' 状态错乱致 JS 泄漏。
    // 失败则留空，回退 m_collectedHtml（等价旧行为）。
    void extractSimplifiedHtml();
    void readSimplifyChunks(int idx, int total, const QString& acc);

    // —— 站点特定 SERP 结构化提取（extract != None 时）——
    // extractAlgorithm==BaiduSerp 等时，注入对应引擎 JS 在 live DOM 结构构化抽取搜索结果，
    // 结果 JSON 存 m_serpJsonResult，透传到 RenderResult.serpJson。失败则留空，降级正常渲染。
    void extractSerp();
    void readSerpChunks(int idx, int total, const QString& acc);

    // —— 搜索引擎 TLS 风控降级（curl 预取 + setHtml 重渲染）——
    // Google/Bing 等基于 TLS 指纹(JA3/JA4) 在握手层 RST Chromium 的 BoringSSL 连接，
    // 而 QNetworkAccessManager(OpenSSL) 不被识别。对搜索引擎 URL，assign 时并行启动预取；
    // Chromium load 失败则用预取 HTML 经 setHtml 重加载（搜索引擎结果高度 JS 化，需 JS 执行）。
    // 预取走 ProxyConfig 配置的代理。非搜索引擎 URL 不启动预取。
    static bool isSearchEngineUrl(const QString& url);
    void startPrefetch(const QString& url);
    void onPrefetched(const QString& html);
    bool tryPrefetchFallback();

    // —— 截图（PNG，所见即所得，区别于 pdf 的打印输出）——
    // 异步状态机：
    //   1) JS 取页全高 H 和视口宽
    //   2) H <= kFullPageMaxPx：视口重置法（resize(W,H) → 等重排 → grab）
    //      H >  kFullPageMaxPx：滚动拼接法（固定视口，逐屏 scrollTo → grab → 贴画布）
    //   3) 编码 PNG → m_collectedImage → decCollect()
    // 含重排/滚动等待，走 QTimer 推进，不阻塞事件循环，参与 m_pendingCollect 计数。
    void captureScreenshot();
    // JS 取得页面尺寸后的回调，按高度分流到重置法/拼接法。
    void onScreenshotSizeKnown(int pageWidth, int pageHeight);
    // 视口重置法：resize 到全高，等重排后 grab。
    void captureByFullPageResize(int pageWidth, int pageHeight);
    void onFullPageResizeSettled(int pageWidth, int pageHeight);  // resize 后重排完成的 grab
    // 滚动拼接法：逐屏滚动 grab 贴画布。
    void startScrollStitch(int pageWidth, int pageHeight);
    void captureNextScrollStitch(int pageWidth, int pageHeight);  // 单屏 grab + 贴画布 + 推进
    // PNG 编码 + 存入 m_collectedImage + 计数收尾（两种路径共用）。
    void finalizeScreenshot(const QImage& img);

    // 截图重置法 vs 拼接法的分流阈值（像素），取 GPU 纹理上限 8192 的保守值。
    static constexpr int kFullPageMaxPx = 8000;
    // 滚动拼接视口高度（每屏高度）。
    static constexpr int kStitchViewportH = 1080;
    // 截图总高度上限。超过则只截前 N 屏并标注 truncated，防无限滚动页无止境拼接。15 屏 × 1080。
    static constexpr int kStitchMaxHeight = kStitchViewportH * 15;
    // 截图宽度上限。页面宽度取自 clientWidth（页面 JS 可控），不加钳制则恶意页报超大宽度会令
    // resize 与 QImage 分配爆炸式内存（OOM DoS）。4096 覆盖所有正常宽页。
    static constexpr int kMaxImageWidth = 4096;
    // 预取 HTML 体积上限。搜索引擎结果页远小于此；超限视为异常/攻击，丢弃降级。
    static constexpr int kMaxPrefetchBytes = 5 * 1024 * 1024;

    QWebEngineProfile* m_profile;
    RenderQueue* m_queue;
    qint64 m_loadTimeoutMsec;
    QWebEngineView* m_view;     // 当前任务专属 view（持有 page）
    QWebEnginePage* m_page;     // m_view->page()，缓存便于连信号；view 销毁时随之失效

    QTimer m_loadTimer;         // 从 assign 起，覆盖 load + 采集的总超时
    QTimer m_settleTimer;       // loadFinished 后的 JS 执行延时

    // 搜索引擎预取降级（见 startPrefetch/tryPrefetchFallback）。QProcess 调 system curl
    //（this 父子管理生命周期，无需单独成员持有）。
    QString m_prefetchedHtml;                        // 预取成功后的 HTML（空=未预取/失败）
    QString m_effectivePrefetchUrl;                  // curl -L 跟随重定向后的最终 URL（同源校验用）
    bool m_prefetchDone = false;                     // 预取已完成（成功或失败）
    bool m_prefetchUsed = false;                     // setHtml 降级已用过（防无限循环）

    RenderTaskPtr m_task;
    bool m_busy;
    bool m_collectStarted;      // 是否已发起采集（防止重入）

    // 采集协调：toHtml/printToPdf 都是异步，需等全部返回再收尾（page 才能销毁）。
    int m_pendingCollect{0};    // 还在等待的异步采集数量
    QString m_collectedHtml;    // 已采集的 HTML（html 输出 + markdown 转换依赖）
    QByteArray m_collectedPdf;  // 已采集的 PDF
    // Readability 抽取的正文 HTML（仅 mdAlgorithm==Readability 时填充）。
    // 空则表示未启用或抽取失败，tryFinishCollect 回退到 m_collectedHtml（保守策略）。
    QString m_readabilityHtml;
    // extract.js（Readability 包装器）内容缓存，首次使用时从磁盘加载，之后复用。
    static QString s_readabilityJs;
    // conservative 路径：DOM 简化后的 HTML（仅 mdAlgorithm==Conservative 且 wants(Markdown) 时填充）。
    // 空则表示未启用或简化失败，tryFinishCollect 回退到 m_collectedHtml（等价旧行为）。
    QString m_simplifiedHtml;
    // SERP 提取结果 JSON（仅 extractAlgorithm==BaiduSerp 时填充）。
    // 空则表示未启用或提取失败，tryFinishCollect 不透传 serpJson（降级正常渲染）。
    QString m_serpJsonResult;
public:
    // 视口是否用 stealth 统一尺寸（由 RenderPool::start 设置一次，所有 worker 共享）。
    // public static：RenderPool 需在 start() 设置，worker 在 assign() 读取。
    static bool s_stealthViewport;

private:
    // simplify.js（DOM 简化器）内容缓存，首次使用时从磁盘加载，之后复用。
    static QString s_simplifyJs;
    // baidu_serp.js / bing_serp.js 内容缓存，按引擎名（baidu_serp/bing_serp）缓存。
    // 首次使用时从磁盘加载，之后复用。
    static QHash<QString, QString> s_serpJsCache;

    // 截图相关状态（仅 captureScreenshot 流程期间有效）。
    QByteArray m_collectedImage;      // 已采集的图片字节（PNG 或 JPEG，编码后）
    QImage m_stitchCanvas;            // 滚动拼接用的全页画布
    int m_stitchY{0};                 // 滚动拼接：当前已贴到画布的 y 偏移
    int m_stitchViewportH{kStitchViewportH};  // 滚动拼接视口高（可能因末屏缩短）
    // 截图最终编码格式。Auto 在 captureScreenshot 里按 img 像素占比解析为 Png/Jpeg。
    ImageFormat m_resolvedImageFmt{ImageFormat::Png};
    // 截图是否因超过高度上限而被截断（仅截前 kStitchMaxHeight 像素）。
    bool m_imageTruncated{false};
};


// 渲染池：GUI 线程持有 N 个 RenderWorker，定时器轮询队列分发。
// 并发来自 N 个 page 实例共享同一 GUI 事件循环（Chromium 多进程提供真正并行）。
class RenderPool : public QObject {
    Q_OBJECT
public:
    RenderPool(RenderQueue* queue, int concurrency, qint64 loadTimeoutMsec, QObject* parent = nullptr);

    void start();   // 在 GUI 线程调用：建 page/worker + 启动分发定时器
    void stop();    // 停止分发（配合 queue->stop）

    // 绑定 cookie 同步中枢：start() 后会被周期 apply 到共享 profile。
    // 可空（不调用则不启用 cookie 同步）。
    void setCookieStore(CookieStore* cs) { m_cookies = cs; }

    // 绑定代理配置中枢：start() 后会被周期 apply 到 QNetworkProxy::setApplicationProxy
    //（Qt WebEngine 轮询它，运行时动态生效）。可空。
    void setProxyConfig(ProxyConfig* pc) { m_proxyConfig = pc; }

    // 开启/关闭浏览器指纹统一（stealth）。开启后 start() 注册 stealth.js（DocumentCreation 注入）
    // + 统一 UA/窗口尺寸。默认开启（--no-stealth 关闭）。必须在 start() 前调用。
    void setStealthEnabled(bool enabled) { m_stealthEnabled = enabled; }
    bool isStealthEnabled() const { return m_stealthEnabled; }

signals:
    // 透传每个 worker 的任务完成事件。
    void taskFinished(const QString& id);

private slots:
    void dispatch();

private:
    RenderQueue* m_queue;
    int m_concurrency;
    qint64 m_loadTimeoutMsec;
    std::vector<std::unique_ptr<RenderWorker>> m_workers;
    QTimer m_dispatchTimer;
    QTimer m_cookieApplyTimer;  // 周期把待注入 cookie apply 到共享 profile（GUI 线程）
    QTimer m_proxyApplyTimer;   // 周期把待生效代理 apply 到 setApplicationProxy（GUI 线程）
    QWebEngineProfile* m_profile; // 所有 worker 共享一个 profile，降低内存/复用缓存
    CookieStore* m_cookies = nullptr;
    ProxyConfig* m_proxyConfig = nullptr;
    bool m_stealthEnabled = true;  // 浏览器指纹统一（默认开启，--no-stealth 关闭）
    // SSRF 二次防护：装到共享 profile 上，对 Chromium 实际发起的每个请求再判一次，
    // 堵住重定向 / DNS rebinding / 内嵌子框架等提交时校验无法覆盖的旁路（见 SsrfInterceptor.h）。
    SsrfRequestInterceptor* m_ssrfInterceptor = nullptr;
};

} // namespace seimi
