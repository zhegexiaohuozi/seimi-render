// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "HttpServer.h"

#include "CookieStore.h"
#include "Environment.h"
#include "FetchService.h"
#include "HarStore.h"
#include "McpServer.h"
#include "ProxyConfig.h"
#include "IpNet.h"
#include "StealthProfile.h"
#include "TokenCompare.h"
#include "UrlGuard.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkCookie>
#include <QUrl>

#include <cerrno>
#include <cstdlib>

#include "httplib.h"

namespace seimi {

static qint64 nowMsec() { return QDateTime::currentMSecsSinceEpoch(); }

// 解析 output 规格 -> OutputMask。
// 接受：逗号分隔字符串 "html,image,markdown"，或 QJsonArray ["html","image"]。
// 空则默认 html。未知项忽略。
static OutputMask parseOutputs(const QJsonValue& v) {
    OutputMask mask = 0;
    auto add = [&](const QString& tok) {
        QString t = tok.trimmed().toLower();
        if (t == QStringLiteral("html"))     mask |= static_cast<OutputMask>(Output::Html);
        else if (t == QStringLiteral("markdown") || t == QStringLiteral("md"))
                                              mask |= static_cast<OutputMask>(Output::Markdown);
        else if (t == QStringLiteral("pdf"))  mask |= static_cast<OutputMask>(Output::Pdf);
        else if (t == QStringLiteral("screenshot") || t == QStringLiteral("image")
                 || t == QStringLiteral("png") || t == QStringLiteral("screenshot_png"))
                                              mask |= static_cast<OutputMask>(Output::Screenshot);
    };
    if (v.isString()) {
        for (const QString& tok : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) add(tok);
    } else if (v.isArray()) {
        for (const QJsonValue& e : v.toArray()) if (e.isString()) add(e.toString());
    }
    if (mask == 0) mask = static_cast<OutputMask>(Output::Html); // 默认 html
    return mask;
}

// 解析截图编码格式：png/jpg/jpeg/auto。默认 auto（智能选择）。
// 未知值也归为 auto（宽容处理，不因格式名拼错而拒绝请求）。
static ImageFormat parseImageFormat(const QJsonValue& v) {
    QString t = v.toString().trimmed().toLower();
    if (t == QStringLiteral("png"))              return ImageFormat::Png;
    if (t == QStringLiteral("jpg") || t == QStringLiteral("jpeg"))
                                                 return ImageFormat::Jpeg;
    return ImageFormat::Auto;  // 含空值/未知值
}

// 解析 markdown 正文提取算法：conservative/readability。默认 conservative。
// conservative=零误伤保守策略（只忽略 script/style/nav/iframe）；
// readability=Mozilla Readability 正文定位（质量高但概率性，不保证零误伤）。
static MdAlgorithm parseMdAlgorithm(const QJsonValue& v) {
    QString t = v.toString().trimmed().toLower();
    if (t == QStringLiteral("readability"))      return MdAlgorithm::Readability;
    return MdAlgorithm::Conservative;  // 含空值/未知值
}

// 解析站点特定提取算法：baidu_serp/bing_serp/google_serp。默认 none（不影响现有行为）。
static ExtractAlgorithm parseExtractAlgorithm(const QJsonValue& v) {
    QString t = v.toString().trimmed().toLower();
    if (t == QStringLiteral("baidu_serp") || t == QStringLiteral("baidu"))
        return ExtractAlgorithm::BaiduSerp;
    if (t == QStringLiteral("bing_serp") || t == QStringLiteral("bing"))
        return ExtractAlgorithm::BingSerp;
    if (t == QStringLiteral("google_serp") || t == QStringLiteral("google"))
        return ExtractAlgorithm::GoogleSerp;
    return ExtractAlgorithm::None;
}

// 简易 JSON 字符串转义（避免在 httplib handler 里混用 Qt JSON 与 std::string）。
static std::string esc(const QString& s) {
    QByteArray u = s.toUtf8();
    std::string out;
    out.reserve(u.size() + 8);
    for (unsigned char c : u) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    return out;
}

// JSON 字符串转义（std::string 版本，供 host 字段用）。
static std::string escJson(const std::string& in) {
    return esc(QString::fromStdString(in));
}

// 把毫秒时长格式化为人类可读的 "1d 02:03:04" 形式（供 uptime_human）。
static std::string humanDuration(std::int64_t ms) {
    if (ms <= 0) return "0s";
    std::int64_t totalSec = ms / 1000;
    std::int64_t days = totalSec / 86400;
    std::int64_t rem = totalSec % 86400;
    std::int64_t h = rem / 3600;
    std::int64_t mn = (rem % 3600) / 60;
    std::int64_t sc = rem % 60;
    char buf[48];
    if (days > 0) {
        std::snprintf(buf, sizeof(buf), "%lldd %02lld:%02lld:%02lld",
                      (long long)days, (long long)h, (long long)mn, (long long)sc);
    } else {
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
                      (long long)h, (long long)mn, (long long)sc);
    }
    return std::string(buf);
}

HttpServer::HttpServer(RenderQueue* queue, int settleDefaultMs, QObject* parent)
    : QObject(parent)
    , m_queue(queue)
    , m_settleDefaultMs(settleDefaultMs) {}

HttpServer::HttpServer(RenderQueue* queue, CookieStore* cookies, int settleDefaultMs, QObject* parent)
    : QObject(parent)
    , m_queue(queue)
    , m_cookies(cookies)
    , m_settleDefaultMs(settleDefaultMs) {}

HttpServer::HttpServer(RenderQueue* queue, CookieStore* cookies, int settleDefaultMs,
                       std::string adminUiDir, std::string password,
                       QObject* parent)
    : QObject(parent)
    , m_queue(queue)
    , m_cookies(cookies)
    , m_settleDefaultMs(settleDefaultMs)
    , m_adminUiDir(std::move(adminUiDir))
    , m_password(std::move(password)) {
    // token = sha256("seimi-render:" + password)，确定性派生：密码不变则 token 不变，
    // 重启/换机器都不变，API 客户端无需每次重启改配置。
    if (!m_password.empty()) {
        QByteArray input = "seimi-render:" + QByteArray::fromStdString(m_password);
        m_token = QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex().toStdString();
    }
}

HttpServer::~HttpServer() {
    stop();
    // stop() 已等 httplib 线程收尾，再回收 fetch 专职线程（顺序保证 execute 不在途）。
    delete m_fetch;
    m_fetch = nullptr;
}

std::string HttpServer::computeToken(const std::string& password) {
    if (password.empty()) return std::string();
    QByteArray input = "seimi-render:" + QByteArray::fromStdString(password);
    return QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex().toStdString();
}

void HttpServer::setTrustedProxies(const std::string& csv) {
    m_trustedProxies = parseIpNetList(csv);
}

bool HttpServer::start(const std::string& host, int port, int threadCount) {
    m_srv = std::make_unique<httplib::Server>();
    // /fetch 直取执行器（必须在 registerRoutes 前、各 setter 之后创建；非 QObject，
    // 由析构显式回收，内部自带专职 QThread）。
    m_fetch = new FetchService(m_proxy);
    m_srv->new_task_queue = [threadCount] {
        // httplib 内置线程池：每个连接由线程池里一个线程处理。
        return new httplib::ThreadPool(threadCount);
    };
    // 请求体大小上限（DoS 防护）。带 body 的路由只收小 JSON，合法请求远不到 1MB。
    // 2MB 给 cookie 批量注入留余量，超限在 httplib 层 413 拒绝。
    m_srv->set_payload_max_length(2 * 1024 * 1024); // 2MB
    // 挂载管理 UI 静态资源到 /ui（index.html 由 GET / 直接返回）。
    if (!m_adminUiDir.empty()) {
        m_srv->set_mount_point("/ui", m_adminUiDir);
        // 预加载 index.html：listen 前单线程完成，避免 GET / 在工作线程池并发命中
        // 惰性初始化（m_adminIndexCache 是 std::string，无锁并发赋值是数据竞争）。
        loadAdminIndex();
    }

    // 全局安全响应头。CSP 对管理页生效：app.js 外链（script-src 'self'）+ 内联 style
    //（style-src 需 'unsafe-inline'）+ data: 占位图（img-src 加 data:）。渲染预览用
    // sandbox iframe srcdoc（内联内容，非网络加载），不受 frame-src 约束。
    // CSP 对 application/json 等非 HTML 响应被浏览器忽略，不影响 API 客户端。
    m_srv->set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");          // 管理页禁止被 iframe 嵌套
        res.set_header("Referrer-Policy", "no-referrer");
        res.set_header("Content-Security-Policy",
            "default-src 'self'; "
            "img-src 'self' data:; "
            "style-src 'self' 'unsafe-inline'; "
            "script-src 'self'; "
            "connect-src 'self'; "
            "frame-src 'none'; "
            "object-src 'none'; base-uri 'none'");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    registerRoutes();

    if (!m_srv->listen(host, port)) return false;
    return true; // 正常路径：listen 阻塞至 stop
}

void HttpServer::stop() {
    if (m_srv) {
        m_srv->stop();
        m_running.store(false);
    }
}

std::string HttpServer::jsonStateResp(const RenderQueue::Snapshot& s, OutputMask outMask) {
    if (!s.found) {
        return R"({"error":"task not found"})";
    }
    qint64 now = nowMsec();
    qint64 start = s.startedAtMsec ? s.startedAtMsec : now;
    qint64 elapsed = (s.done && s.finishedAtMsec)
                         ? (s.finishedAtMsec - start)
                         : (now - start);

    std::string out = "{\"task_id\":\"" + esc(s.id) +
                      "\",\"url\":\"" + esc(s.url) +
                      "\",\"state\":\"" + taskStateName(s.state) +
                      "\"";
    if (s.done && s.state == TaskState::Failed) {
        out += ",\"error\":\"" + esc(s.error) + "\"";
        if (s.blocked) out += ",\"blocked\":true";   // 反爬拦截类型化标记（C3）
    }
    // final_url：重定向/SSO/登录墙跳转的可见性载体（成功与失败路径都可能有值；
    // 失败时多为 chrome-error 页或超时瞬间的 URL）。
    if (!s.finalUrl.isEmpty()) {
        out += ",\"final_url\":\"" + esc(s.finalUrl) + "\"";
    }
    out += ",\"elapsed_ms\":" + std::to_string(elapsed < 0 ? 0 : elapsed);

    // 成功时按 outMask 返回对应输出。html/markdown 内嵌 JSON；pdf 给元信息
    // （实际 PDF 通过 GET /pdf/:id 拉取，避免 base64 撑爆 JSON）。
    if (s.state == TaskState::Succeeded) {
        if ((outMask & static_cast<OutputMask>(Output::Html)) && !s.html.isEmpty()) {
            out += ",\"html\":\"" + esc(s.html) + "\"";
        }
        if ((outMask & static_cast<OutputMask>(Output::Markdown)) && !s.markdown.isEmpty()) {
            out += ",\"markdown\":\"" + esc(s.markdown) + "\"";
            // 可观测性：实际使用的算法。请求 readability 但非文章页时会回退 raw/conservative。
            out += ",\"md_algorithm_used\":\"" + std::string(mdAlgorithmName(s.mdAlgorithmUsed)) + "\"";
        }
        if ((outMask & static_cast<OutputMask>(Output::Pdf)) && !s.pdfData.isEmpty()) {
            out += ",\"has_pdf\":true,\"pdf_bytes\":" + std::to_string(s.pdfData.size());
            out += ",\"pdf\":\"/pdf/" + s.id.toStdString() + "\"";
        }
        if ((outMask & static_cast<OutputMask>(Output::Screenshot)) && !s.imageData.isEmpty()) {
            out += ",\"has_image\":true,\"image_bytes\":" + std::to_string(s.imageData.size());
            out += ",\"image_format\":\"" + std::string(
                s.resolvedImageFmt == ImageFormat::Jpeg ? "jpeg" : "png") + "\"";
            if (s.imageTruncated) out += ",\"image_truncated\":true";
            out += ",\"image\":\"/image/" + s.id.toStdString() + "\"";
        }
        // SERP 结构化提取结果（裸嵌入：serpJson 本身是合法 JSON 对象字符串，
        // 不能过 esc() 否则破坏内部结构）。
        if (!s.serpJson.isEmpty()) {
            out += ",\"serp_json\":" + s.serpJson.toStdString();
        }
        // exec_js 结果（裸嵌入合法 JSON 值文本：可能是对象/数组/字符串/数字）。
        // 未请求 exec_js 或脚本无返回值（void）时不出现该字段。
        if (!s.jsJson.isEmpty()) {
            out += ",\"js_result\":" + s.jsJson.toStdString();
        }
        // 登录墙启发式：成功渲染出的"登录页"（session 过期等）与正常内容可区分，
        // agent 据此提示重新同步 cookie 而非把登录页 markdown 当业务数据继续分析。
        if (s.authWall) {
            out += ",\"auth_wall\":true";
            if (!s.authWallReason.isEmpty()) {
                out += ",\"auth_wall_reason\":\"" + esc(s.authWallReason) + "\"";
            }
        }
    }
    out += "}";
    return out;
}

// 运行时状态全景 JSON（GET /status）。
// 用纯 std::string 拼接（与本项目其它 handler 一致，避免在 httplib handler 里混 Qt JSON）。
std::string HttpServer::jsonRuntimeStatus(const httplib::Request& req) {
    // 域名条数限制：?domains=N，默认 20，上限 200。
    int maxDomains = 20;
    if (req.has_param("domains")) {
        int v = std::atoi(req.get_param_value("domains").c_str());
        if (v > 0) maxDomains = std::min(v, 200);
    }

    Metrics::Snapshot m = m_queue->metrics().snapshot(maxDomains);
    RenderQueue::Stats q = m_queue->stats();

    auto fmtMs = [](std::int64_t v) { return std::to_string(v); };
    auto fmtDouble = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return std::string(buf);
    };

    std::string s = "{";
    // —— 进程信息 ——
    s += "\"started_at_ms\":" + fmtMs(m.startedAtMsec);
    s += ",\"uptime_ms\":" + fmtMs(m.uptimeMsec);
    s += ",\"uptime_human\":\"" + humanDuration(m.uptimeMsec) + "\"";

    // —— 当前队列快照（实时）——
    // workers/workers_busy/peak_pending：渲染负载可观测。
    //   workers      = 并发槽位总数（--concurrency，静态）
    //   workers_busy = 正在渲染的 worker 数（= running 任务数）
    //   peak_pending = pending 队列历史最大堆积长度（单调不降）
    s += ",\"queue\":{\"total\":" + std::to_string(q.total) +
         ",\"pending\":" + std::to_string(q.pending) +
         ",\"running\":" + std::to_string(q.running) +
         ",\"done\":" + std::to_string(q.done) +
         ",\"workers\":" + std::to_string(q.concurrency) +
         ",\"workers_busy\":" + std::to_string(q.busy) +
         ",\"peak_pending\":" + std::to_string(q.peakPending) + "}";

    // —— 当前网络代理（运行时生效配置）——
    // 与 GET /proxy 同源（都读 ProxyConfig::snapshot），密码不回显。
    // m_proxy 为空（未启用 /proxy 路由）时报告 disabled，便于 UI 统一渲染。
    s += ",\"proxy\":";
    if (m_proxy) {
        ProxySpec p = m_proxy->snapshot();
        std::string type = p.type == ProxySpec::Type::Http ? "http"
                         : p.type == ProxySpec::Type::Socks5 ? "socks5" : "direct";
        s += "{\"enabled\":" + std::string(p.type == ProxySpec::Type::Direct ? "false" : "true");
        s += ",\"type\":\"" + type + "\"";
        if (p.type != ProxySpec::Type::Direct) {
            s += ",\"host\":\"" + escJson(p.host) + "\"";
            s += ",\"port\":" + std::to_string(p.port);
            s += ",\"user\":\"" + escJson(p.user) + "\"";
            s += ",\"password_set\":" + std::string(p.pass.empty() ? "false" : "true");
        }
        s += "}";
    } else {
        s += "{\"enabled\":false,\"type\":\"direct\"}";
    }

    // —— 累计计数（自启动）——
    s += ",\"totals\":{\"requests\":" + std::to_string(m.total) +
         ",\"succeeded\":" + std::to_string(m.succeeded) +
         ",\"failed\":" + std::to_string(m.failed) +
         ",\"blocked_total\":" + std::to_string(m.blockedTotal) +
         ",\"blocked_recovered\":" + std::to_string(m.blockedRecovered) +
         ",\"blocked_exhausted\":" + std::to_string(m.blockedExhausted) +
         ",\"success_rate\":" + fmtDouble(m.successRate) + "}";

    // —— 延迟分布（仅成功任务，渲染耗时）——
    s += ",\"latency_ms\":{\"min\":" + fmtMs(m.latencyMinMs) +
         ",\"avg\":" + fmtDouble(m.latencyAvgMs) +
         ",\"p50\":" + fmtMs(m.latencyP50Ms) +
         ",\"p90\":" + fmtMs(m.latencyP90Ms) +
         ",\"p99\":" + fmtMs(m.latencyP99Ms) +
         ",\"max\":" + fmtMs(m.latencyMaxMs) + "}";

    // —— 吞吐 ——
    s += ",\"throughput_per_sec\":" + fmtDouble(m.throughputPerSec);

    // —— 输出类型需求分布 ——
    s += ",\"outputs\":{\"html\":" + std::to_string(m.outputHtml) +
         ",\"markdown\":" + std::to_string(m.outputMarkdown) +
         ",\"pdf\":" + std::to_string(m.outputPdf) +
         ",\"screenshot\":" + std::to_string(m.outputScreenshot) + "}";

    // —— 域名分布 ——
    s += ",\"domains\":{";
    s += "\"distinct\":" + std::to_string(m.distinctDomains);
    s += ",\"top\":[";
    for (std::size_t i = 0; i < m.domains.size(); ++i) {
        if (i) s += ",";
        s += "{\"domain\":\"" + escJson(m.domains[i].host) + "\"" +
             ",\"total\":" + std::to_string(m.domains[i].total) +
             ",\"succeeded\":" + std::to_string(m.domains[i].succeeded) +
             ",\"failed\":" + std::to_string(m.domains[i].failed) +
             ",\"blocked\":" + std::to_string(m.domains[i].blocked) +
             ",\"success_rate\":" + fmtDouble(
                 m.domains[i].total == 0 ? 0.0
                 : double(m.domains[i].succeeded) / double(m.domains[i].total)) +
             "}";
    }
    s += "]}";

    // —— 运行环境信息（启动时静态采集 + GUI 定时器实时采样）——
    if (m_env) {
        EnvironmentSnapshot e = m_env->snapshot();
        // escJson 接受 std::string；QString 字段先 toStdString()。
        // 用本地 lambda 减少重复，对齐现有 handler 的拼接风格。
        auto esc = [](const QString& q) { return escJson(q.toStdString()); };
        s += ",\"environment\":{";
        s += "\"os_name\":\"" + esc(e.osName) + "\"";
        s += ",\"os_version\":\"" + esc(e.osVersion) + "\"";
        s += ",\"os_pretty\":\"" + esc(e.osPretty) + "\"";
        s += ",\"kernel\":\"" + esc(e.kernel) + "\"";
        s += ",\"arch\":\"" + esc(e.arch) + "\"";
        s += ",\"build_arch\":\"" + esc(e.buildArch) + "\"";
        s += ",\"hostname\":\"" + esc(e.hostname) + "\"";
        s += ",\"cpu_model\":\"" + esc(e.cpuModel) + "\"";
        s += ",\"cpu_cores_logical\":" + std::to_string(e.cpuLogicalCores);
        s += ",\"cpu_cores_physical\":" + std::to_string(e.cpuPhysicalCores);
        s += ",\"memory_total_mb\":" + std::to_string(e.memoryTotalMb);
        s += ",\"has_gpu\":" + std::string(e.hasGpu ? "true" : "false");
        s += ",\"qt_version\":\"" + esc(e.qtVersion) + "\"";
        s += ",\"build_time\":\"" + esc(e.buildTime) + "\"";
        s += ",\"git_commit\":\"" + esc(e.gitCommit) + "\"";
        s += ",\"launch_args\":\"" + esc(e.launchArgs) + "\"";
        s += ",\"cpu_percent\":" + fmtDouble(e.cpuPercent);
        s += ",\"memory_rss_mb\":" + std::to_string(e.memoryRssMb);
        s += ",\"memory_percent\":" + fmtDouble(e.memoryPercent);
        s += ",\"sampled_at_ms\":" + fmtMs(e.sampledAtMs);
        s += ",\"markdown\":\"" + esc(e.markdown) + "\"";
        s += "}";
    }

    // —— MCP 服务健康度 ——
    // 让运维能在 session 堆积到上限前发现问题：sessions_used 持续逼近 sessions_limit
    // 即将触发 503。可空（未启用 MCP 时 m_mcp 为 nullptr，本段不出现）。
    if (m_mcp) {
        s += ",\"mcp\":{";
        s += "\"enabled\":" + std::string(m_mcp->isRunning() ? "true" : "false");
        s += ",\"sessions_used\":" + std::to_string(m_mcp->activeSessionCount());
        s += ",\"sessions_limit\":" + std::to_string(m_mcp->maxSessions());
        s += "}";
    }

    s += "}";
    return s;
}

std::string HttpServer::loadAdminIndex() {
    if (m_adminIndexLoaded) return m_adminIndexCache;
    m_adminIndexLoaded = true;
    QString path = QString::fromStdString(m_adminUiDir) + QStringLiteral("/index.html");
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        m_adminIndexCache = QString::fromUtf8(f.readAll()).toStdString();
    }
    // 静态资源加版本戳（cache-busting）：用启动时间戳作版本号，重启即强制浏览器重拉，
    // 确保管理页 JS/CSS 总是最新的。
    if (!m_adminIndexCache.empty()) {
        static const std::string ver = std::to_string(
            QDateTime::currentSecsSinceEpoch());
        std::string& s = m_adminIndexCache;
        // 替换 /ui/app.css" -> /ui/app.css?v=<ver>"，/ui/app.js" / i18n.js / docs.js 同理
        for (const std::string& asset : {"app.css", "app.js", "i18n.js", "docs.js"}) {
            std::string needle = "/ui/" + asset + "\"";
            std::string repl = "/ui/" + asset + "?v=" + ver + "\"";
            size_t pos = s.find(needle);
            if (pos != std::string::npos) s.replace(pos, needle.size(), repl);
        }
    }
    return m_adminIndexCache;
}

bool HttpServer::checkAuth(const httplib::Request& req) const {
    // 已禁用密码：放行
    if (m_password.empty()) return true;
    // 1) Authorization: Bearer <token>（恒定时间比较）
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end()) {
        const std::string& v = it->second;
        const char* prefix = "Bearer ";
        const size_t plen = 7;  // strlen("Bearer ")
        if (v.size() > plen && v.compare(0, plen, prefix) == 0) {
            if (constantTimeEquals(v.substr(plen), m_token)) return true;
        }
    }
    // 2) query ?token=<token>（便于浏览器直接打开 /pdf/:id 等链接）
    if (req.has_param("token") && constantTimeEquals(req.get_param_value("token"), m_token))
        return true;
    // 3) Cookie: seimi_token=<token>（登录页写入）。按 cookie 分隔精确解析，避免子串匹配绕过。
    if (req.has_header("Cookie")) {
        const std::string& c = req.get_header_value("Cookie");
        std::string key = "seimi_token=";
        size_t pos = 0;
        while (pos < c.size()) {
            size_t sep = c.find(';', pos);
            std::string part = (sep == std::string::npos) ? c.substr(pos)
                                                          : c.substr(pos, sep - pos);
            // 去掉前后空格
            size_t s = part.find_first_not_of(' ');
            size_t e = part.find_last_not_of(' ');
            if (s != std::string::npos) {
                std::string kv = part.substr(s, e - s + 1);
                if (kv.size() > key.size()
                    && kv.compare(0, key.size(), key) == 0
                    && constantTimeEquals(kv.substr(key.size()), m_token)) {
                    return true;
                }
            }
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
    }
    return false;
}

void HttpServer::rejectAuth(httplib::Response& res) {
    res.status = 401;
    res.set_content(R"({"error":"unauthorized","login":"/api/login"})", "application/json; charset=utf-8");
}

void HttpServer::noteLoginFailure(const QString& ip) {
    QMutexLocker locker(&m_loginMutex);
    auto it = m_loginByIp.find(ip);
    if (it == m_loginByIp.end()) {
        // 容量上限防 --trusted-proxy 下伪造 XFF 注入无界 IP 耗尽内存。
        if (static_cast<int>(m_loginByIp.size()) >= kMaxLoginIps) {
            reclaimLoginSlotLocked();
        }
        it = m_loginByIp.insert(ip, LoginRate{});
    }
    ++it->failures;
    if (it->failures >= kLoginMaxFailures) {
        it->lockUntilMs = nowMsec() + kLoginLockMs;
    }
}

// 持锁牺牲最陈旧条目腾槽：lockUntilMs 最小者优先（未锁定/已过期自然先回收，
// 活跃攻击者条目尽量保留其冻结到最后）。O(N)，仅在触顶时调用。
void HttpServer::reclaimLoginSlotLocked() {
    auto victim = m_loginByIp.begin();
    for (auto it = m_loginByIp.begin(); it != m_loginByIp.end(); ++it) {
        if (it.value().lockUntilMs < victim.value().lockUntilMs) victim = it;
    }
    if (victim != m_loginByIp.end()) m_loginByIp.erase(victim);
}

void HttpServer::noteLoginSuccess(const QString& ip) {
    QMutexLocker locker(&m_loginMutex);
    m_loginByIp.remove(ip);
}

qint64 HttpServer::loginLockRemainingMs(const QString& ip) const {
    QMutexLocker locker(&m_loginMutex);
    auto it = m_loginByIp.constFind(ip);
    if (it == m_loginByIp.constEnd()) return 0;
    if (it->lockUntilMs == 0) return 0;
    qint64 rem = it->lockUntilMs - nowMsec();
    return rem > 0 ? rem : 0;
}

void HttpServer::gcLoginRate() {
    // 节流：间隔取锁定时长一半，确保被锁条目至少在锁到期后的一两次 gc 中被回收。
    qint64 now = nowMsec();
    if (now - m_lastLoginGcMs < kLoginGcIntervalMs) return;
    m_lastLoginGcMs = now;
    for (auto it = m_loginByIp.begin(); it != m_loginByIp.end();) {
        const LoginRate& s = it.value();
        // 仅回收「无锁定 且 失败计数为 0」。有失败计数但未触发锁定的保留（防攻击者打 9 次停手清表规避计数）。
        bool lockExpired = (s.lockUntilMs == 0 || s.lockUntilMs <= now);
        if (lockExpired && s.failures == 0) {
            it = m_loginByIp.erase(it);
        } else {
            ++it;
        }
    }
}

// 便捷宏式：启用密码时，对受保护路由先做 token 校验。
// 用法在每个受保护 handler 开头：if (!m_password.empty() && !checkAuth(req)) { rejectAuth(res); return; }
#define SEIMI_AUTH(req, res) do { if (!m_password.empty() && !checkAuth(req)) { rejectAuth(res); return; } } while(0)

void HttpServer::registerRoutes() {
    // 健康/探活检查（始终免鉴权，供监控/容器探活用）。原 GET / 的 JSON 移到这里以保持 API 兼容。
    m_srv->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"service":"seimi-render","status":"ok"})",
                        "application/json; charset=utf-8");
    });

    // 鉴权状态查询（免鉴权）。管理页据此判断密码是否启用（不依赖本地 sessionStorage token 残留）。
    m_srv->Get("/auth-status", [this](const httplib::Request&, httplib::Response& res) {
        std::string body = "{\"password_enabled\":";
        body += (m_password.empty() ? "false" : "true");
        body += "}";
        res.set_content(body, "application/json; charset=utf-8");
    });

    // 管理 UI 登录（仅启用密码时注册）。前端 POST {password}，对则返回 token。
    if (!m_password.empty()) {
        m_srv->Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
            // 还原真实客户端 IP：仅当直连来源命中可信代理时才信任 XFF（同 nginx real_ip）。
            // 未配可信代理则用 TCP 直连地址、忽略 XFF（防伪造）。详见 IpNet.h。
            const QHostAddress remoteAddr(QString::fromStdString(req.remote_addr));
            const QString xff = QString::fromStdString(req.get_header_value("X-Forwarded-For"));
            const QString ip = resolveRealClientIp(remoteAddr, xff, m_trustedProxies);

            // 按 IP 登录限流：该 IP 连续失败达上限后冻结。锁内完全不校验密码（不再给出
            // 对/错信号，在线暴力被掐断）；合法管理员来自别的 IP 不受影响。
            qint64 lockRem = loginLockRemainingMs(ip);
            if (lockRem > 0) {
                res.status = 429;
                int retrySec = int((lockRem + 999) / 1000);
                res.set_header("Retry-After", std::to_string(retrySec));
                std::string body = R"({"error":"too many login failures","retry_after_sec":)"
                                 + std::to_string(retrySec) + R"(})";
                res.set_content(body, "application/json; charset=utf-8");
                return;
            }

            // 解析 {password}
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            QString pwd = (err.error == QJsonParseError::NoError && doc.isObject())
                              ? doc.object().value(QStringLiteral("password")).toString()
                              : QString();
            // 恒定时间比较密码（与 token 同理，避免时序侧信道逐字节枚举密码）。
            if (constantTimeEquals(pwd.toStdString(), m_password)) {
                noteLoginSuccess(ip);  // 成功即清零该 IP 失败计数
                // 签发 HttpOnly cookie（与 body 内 token 同值）：HttpOnly 挡 XSS 读取，
                // SameSite=Lax 挡 CSRF 又放行顶层导航。不设 Secure（本服务直连即 HTTP，
                // TLS 由上游反代终结）。body 仍返回 token 供 API 客户端。
                res.set_header("Set-Cookie",
                    std::string("seimi_token=") + m_token
                    + "; Path=/; HttpOnly; SameSite=Lax");
                res.set_content(std::string("{\"token\":\"") + m_token + "\"}", "application/json; charset=utf-8");
            } else {
                noteLoginFailure(ip);
                res.status = 401;
                res.set_content(R"({"error":"invalid password"})", "application/json; charset=utf-8");
            }
            // 顺带回收过期条目（节流，实际每 5 分钟才扫一次）。
            gcLoginRate();
        });
    }

    // 根路径始终返回管理 UI 首页（含未授权时）。页面本身无敏感数据，真正数据鉴权在各 API。
    // 浏览器直接访问 / 时看到登录页，而非 401 JSON。
    m_srv->Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        if (!m_adminUiDir.empty()) {
            std::string html = loadAdminIndex();
            if (!html.empty()) {
                res.set_content(html, "text/html; charset=utf-8");
                return;
            }
        }
        // 无管理页或读不到：回退健康检查
        res.set_content(R"({"service":"seimi-render","status":"ok"})", "application/json; charset=utf-8");
    });

    // 统计（简单队列快照，保留向后兼容）
    m_srv->Get("/stats", [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        RenderQueue::Stats s = m_queue->stats();
        std::string body = "{\"total\":" + std::to_string(s.total) +
                           ",\"pending\":" + std::to_string(s.pending) +
                           ",\"running\":" + std::to_string(s.running) +
                           ",\"done\":" + std::to_string(s.done) +
                           ",\"workers\":" + std::to_string(s.concurrency) +
                           ",\"workers_busy\":" + std::to_string(s.busy) +
                           ",\"peak_pending\":" + std::to_string(s.peakPending) + "}";
        res.set_content(body, "application/json; charset=utf-8");
    });

    // 运行时状态全景：进程信息 + 累计计数 + 成功率 + 延迟分布(p50/p90/p99) +
    // 吞吐 + 输出类型需求分布 + 域名分布(top-N) + 当前队列快照。
    // 可带 ?domains=N 控制返回的域名条数（默认 20，上限 200）。
    m_srv->Get("/status", [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        res.set_content(jsonRuntimeStatus(req), "application/json; charset=utf-8");
    });

    // ====== Cookie 同步（浏览器插件用）======
    // POST /cookies：批量同步 cookies（仅入内存缓冲，注入由 GUI 线程异步完成）。
    // GET  /cookies：概览（域名→数量，不含 value，防会话泄露）。
    // DELETE /cookies：清空（调 GUI 线程 deleteAllCookies + 清概览）。
    if (m_cookies) {
        // 批量同步
        m_srv->Post("/cookies", [this](const httplib::Request& req, httplib::Response& res) {
            SEIMI_AUTH(req, res);
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                res.status = 400;
                res.set_content(R"({"error":"invalid json body"})", "application/json; charset=utf-8");
                return;
            }
            QJsonArray arr = doc.object().value(QStringLiteral("cookies")).toArray();
            if (arr.isEmpty()) {
                res.status = 400;
                res.set_content(R"({"error":"'cookies' array is empty or missing"})", "application/json; charset=utf-8");
                return;
            }
            // 上限保护：单次 ≤ 5000，防滥用
            const int kMaxPerBatch = 5000;
            std::vector<CookieStore::Cookie> batch;
            batch.reserve(std::min<int>(arr.size(), kMaxPerBatch));
            for (const QJsonValue& v : arr) {
                if (!v.isObject()) continue;
                QJsonObject o = v.toObject();
                CookieStore::Cookie c;
                c.name = o.value(QStringLiteral("name")).toString().toStdString();
                if (c.name.empty()) continue;  // name 必填
                c.value = o.value(QStringLiteral("value")).toString().toStdString();
                c.domain = o.value(QStringLiteral("domain")).toString().toStdString();
                c.path = o.value(QStringLiteral("path")).toString().toStdString();
                if (c.path.empty()) c.path = "/";
                c.hostOnly = o.value(QStringLiteral("hostOnly")).toBool(false);
                c.secure = o.value(QStringLiteral("secure")).toBool(false);
                c.httpOnly = o.value(QStringLiteral("httpOnly")).toBool(false);
                // Chrome 的 expirationDate 是 epoch 秒（浮点），转 qint64。
                c.expirationDate = qint64(o.value(QStringLiteral("expirationDate")).toDouble(0.0));
                batch.push_back(std::move(c));
                if (int(batch.size()) >= kMaxPerBatch) break;
            }
            int stored = m_cookies->record(batch);
            res.set_content("{\"stored\":" + std::to_string(stored) +
                            ",\"applied\":true}", "application/json; charset=utf-8");
        });

        // 概览
        m_srv->Get("/cookies", [this](const httplib::Request& req, httplib::Response& res) {
            SEIMI_AUTH(req, res);
            CookieStore::Overview ov = m_cookies->snapshot(200);
            std::string s = "{\"total\":" + std::to_string(ov.total) + ",\"domains\":[";
            for (std::size_t i = 0; i < ov.domains.size(); ++i) {
                if (i) s += ",";
                s += "{\"domain\":\"" + escJson(ov.domains[i].domain) +
                     "\",\"count\":" + std::to_string(ov.domains[i].count) + "}";
            }
            s += "]}";
            res.set_content(s, "application/json; charset=utf-8");
        });

        // 清空
        m_srv->Delete("/cookies", [this](const httplib::Request& req, httplib::Response& res) {
            SEIMI_AUTH(req, res);
            if (req.has_param("permanent")) {
                // 永久删除：删除 data/cookies.dat 文件 + 清全部内存状态。
                m_cookies->requestPurge();
                res.set_content(R"({"purged":true})", "application/json; charset=utf-8");
            } else {
                // 普通清空：清内存 + 清持久仓库 + 落空文件（重启不恢复）。
                m_cookies->requestClear();
                res.set_content(R"({"cleared":true})", "application/json; charset=utf-8");
            }
        });
    }

    // ====== 网络代理动态配置（ProxyConfig）======
    // GET    /proxy  → 当前代理配置快照（密码不回显）
    // POST   /proxy  ← {type,host,port,user,pass} 设置代理（运行时动态生效）
    // DELETE /proxy  → 恢复直连（等价 type=direct）
    if (m_proxy) {
        m_srv->Get("/proxy", [this](const httplib::Request& req, httplib::Response& res) {
            SEIMI_AUTH(req, res);
            ProxySpec p = m_proxy->snapshot();
            // 密码绝不回显（仅 user 便于核对账号是否配对）
            std::string type = p.type == ProxySpec::Type::Http ? "http"
                             : p.type == ProxySpec::Type::Socks5 ? "socks5" : "direct";
            std::string s = "{\"type\":\"" + type + "\"";
            if (p.type != ProxySpec::Type::Direct) {
                s += ",\"host\":\"" + escJson(p.host) + "\"";
                s += ",\"port\":" + std::to_string(p.port);
                s += ",\"user\":\"" + escJson(p.user) + "\"";
                s += ",\"password_set\":" + std::string(p.pass.empty() ? "false" : "true");
            }
            s += "}";
            res.set_content(s, "application/json; charset=utf-8");
        });

        m_srv->Post("/proxy", [this](const httplib::Request& req, httplib::Response& res) {
            SEIMI_AUTH(req, res);
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                res.status = 400;
                res.set_content(R"({"error":"invalid json body"})", "application/json; charset=utf-8");
                return;
            }
            QJsonObject obj = doc.object();
            QString typeStr = obj.value(QStringLiteral("type")).toString().trimmed().toLower();
            ProxySpec spec;
            if (typeStr == QStringLiteral("direct") || typeStr.isEmpty()) {
                spec.type = ProxySpec::Type::Direct;
            } else if (typeStr == QStringLiteral("http")) {
                spec.type = ProxySpec::Type::Http;
            } else if (typeStr == QStringLiteral("socks5") || typeStr == QStringLiteral("socks")) {
                spec.type = ProxySpec::Type::Socks5;
            } else {
                res.status = 400;
                res.set_content(R"({"error":"'type' must be direct|http|socks5"})", "application/json; charset=utf-8");
                return;
            }
            if (spec.type != ProxySpec::Type::Direct) {
                spec.host = obj.value(QStringLiteral("host")).toString().trimmed().toStdString();
                // 兼容 port 作为 JSON 字符串发送（部分客户端会发 "7890"，Qt toInt 对字符串返回 0）。
                const QJsonValue portVal = obj.value(QStringLiteral("port"));
                int port = 0;
                if (portVal.isDouble()) {
                    port = int(portVal.toDouble());
                } else if (portVal.isString()) {
                    bool ok = false;
                    int p = portVal.toString().trimmed().toInt(&ok);
                    if (ok) port = p;
                }
                spec.port = quint16(port);
                spec.user = obj.value(QStringLiteral("user")).toString().toStdString();
                spec.pass = obj.value(QStringLiteral("pass")).toString().toStdString();
                if (spec.host.empty() || port < 1 || port > 65535) {
                    res.status = 400;
                    res.set_content(R"({"error":"http/socks5 requires 'host' and 'port' [1-65535]"})", "application/json; charset=utf-8");
                    return;
                }
            }
            m_proxy->setProxy(spec);
            std::string respType = spec.type == ProxySpec::Type::Http ? "http"
                                 : spec.type == ProxySpec::Type::Socks5 ? "socks5" : "direct";
            std::string s = "{\"ok\":true,\"type\":\"" + respType + "\"";
            if (spec.type != ProxySpec::Type::Direct) {
                s += ",\"host\":\"" + escJson(spec.host) + "\"";
                s += ",\"port\":" + std::to_string(spec.port);
            }
            s += "}";
            res.set_content(s, "application/json; charset=utf-8");
        });

        m_srv->Delete("/proxy", [this](const httplib::Request& req, httplib::Response& res) {
            SEIMI_AUTH(req, res);
            ProxySpec direct;  // 默认 Direct
            m_proxy->setProxy(direct);
            res.set_content(R"({"ok":true,"type":"direct"})", "application/json; charset=utf-8");
        });
    }

    // 提交渲染任务（支持可选长轮询）
    m_srv->Post("/render", [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(req.body), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            res.status = 400;
            res.set_content(R"({"error":"invalid json body"})", "application/json; charset=utf-8");
            return;
        }
        QJsonObject obj = doc.object();
        QString url = obj.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty()) {
            res.status = 400;
            res.set_content(R"({"error":"missing 'url'"})", "application/json; charset=utf-8");
            return;
        }
        // 基础 URL 校验
        QUrl qurl(url);
        if (!qurl.isValid() || (qurl.scheme() != QStringLiteral("http")
                                && qurl.scheme() != QStringLiteral("https"))) {
            res.status = 400;
            res.set_content(R"({"error":"url must be http/https"})", "application/json; charset=utf-8");
            return;
        }
        // resolved IP：随任务存一份，网络类失败时附进错误信息做自诊断（proxy/fake-ip
        // hint；agent 读错误即可自修复）。--no-ssrf 下不做判定，但仍解析一次（诊断价值
        // 大于毫秒级 DNS 开销）。
        std::string resolvedIp;
        if (m_ssrfGuardEnabled) {
            std::string ssrfReason = urlSsrfCheck(qurl, &resolvedIp);
            if (!ssrfReason.empty()) {
                // 入口拒绝也是请求异常（客户端视角即失败）：记入异常日志，供统一分析。
                m_queue->anomalyLog().recordRejected(
                    url, QStringLiteral("ssrf_blocked"), QString::fromStdString(ssrfReason));
                res.status = 400;
                std::string body = R"({"error":"url blocked by SSRF guard","detail":")"
                                 + escJson(ssrfReason) + R"("})";
                res.set_content(body, "application/json; charset=utf-8");
                return;
            }
        } else {
            resolvedIp = resolveHostIp(qurl);
        }

        int settleMs = int(obj.value(QStringLiteral("settle_ms")).toInt(m_settleDefaultMs));
        settleMs = std::clamp(settleMs, 0, 30000); // 上限 30s，防滥用
        unsigned long longPollMs = unsigned(obj.value(QStringLiteral("long_poll_ms")).toInt(0));
        OutputMask outputs = parseOutputs(obj.value(QStringLiteral("output")));
        // 截图编码格式：png/jpg/jpeg/auto。默认 auto（按图片像素占比智能选择）。
        ImageFormat imgFmt = parseImageFormat(obj.value(QStringLiteral("format")));
        // markdown 正文提取算法：conservative/readability。默认 conservative（零误伤）。
        MdAlgorithm mdAlg = parseMdAlgorithm(obj.value(QStringLiteral("md_algorithm")));
        // 站点特定提取算法：baidu_serp（百度搜索结果页结构化提取）。默认 none。
        ExtractAlgorithm extractAlg = parseExtractAlgorithm(obj.value(QStringLiteral("extract")));
        // HAR 网络录制：record_network=true 开启该任务的 CDP 网络录制，结果走 GET /har/<id>。
        const bool recordHar = obj.value(QStringLiteral("record_network")).toBool(false);
        // exec_js：settle 后在页面主世界执行调用方 JS（结果 JSON 内嵌 js_result 字段）。
        // 代码体上限 1MB（防滥用）；超时 1~30s 钳制（页面内 Promise.race + C++ 轮询双兜底）。
        QString execJs = obj.value(QStringLiteral("exec_js")).toString();
        if (execJs.size() > 1024 * 1024) {
            res.status = 400;
            res.set_content(R"({"error":"exec_js too large, limit 1MB"})",
                            "application/json; charset=utf-8");
            return;
        }
        int execJsTimeout = obj.value(QStringLiteral("exec_js_timeout_ms")).toInt(10000);
        execJsTimeout = std::clamp(execJsTimeout, 1000, 30000);

        QString id = m_queue->submit(url, settleMs, outputs, imgFmt, mdAlg, nowMsec(),
                                     extractAlg, recordHar, execJs, execJsTimeout,
                                     QString::fromStdString(resolvedIp));
        if (id.isEmpty()) {
            // 任务表过载（洪泛提交/积压）：背压拒绝，让客户端稍后重试。
            res.status = 503;
            res.set_header("Retry-After", "2");
            res.set_content(R"({"error":"server overloaded, retry later"})", "application/json; charset=utf-8");
            return;
        }

        if (longPollMs == 0) {
            // 非长轮询：立即返回 pending。用持锁快照读字段（避免跨线程数据竞争）。
            res.set_content(jsonStateResp(m_queue->snapshot(id), outputs), "application/json; charset=utf-8");
            return;
        }

        // 长轮询：阻塞当前 httplib 工作线程，最多 longPollMs。
        longPollMs = std::min<unsigned long>(longPollMs, 60000UL); // 上限 60s
        m_queue->waitForCompletion(id, longPollMs);  // 仅阻塞等待；字段读取走快照
        RenderQueue::Snapshot s = m_queue->snapshot(id);
        if (!s.found) {
            res.status = 404;
            res.set_content(R"({"error":"task not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(jsonStateResp(s, s.done ? outputs : 0), "application/json; charset=utf-8");
    });

    // 轻量直取端点（绕过 Chromium 页面管线）：带登录态 cookie + 全局代理直接请求
    // JSON/REST 端点，返回原始响应体。场景：agent 多轮 API 探索（Jira /search 等）——
    // 整页渲染管线（新建 page → load → settle → toHtml）单请求 5-6s，这里压到百毫秒级，
    // 且无需从 <pre> 里反解 JSON。网络栈用 QNetworkAccessManager：自动跟随全局代理
    //（http/socks5）与 --proxy-bypass 分流，与页面渲染网络路径一致，行为可互相印证。
    // 跑在 httplib 工作线程：栈上 QEventLoop 同步等待（HarRecorder::discoverTargetId 同款
    // 先例），不碰 GUI 线程与 WebEngine。
    m_srv->Post("/fetch", [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(req.body), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            res.status = 400;
            res.set_content(R"({"error":"invalid json body"})", "application/json; charset=utf-8");
            return;
        }
        QJsonObject obj = doc.object();
        QString url = obj.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty()) {
            res.status = 400;
            res.set_content(R"({"error":"missing 'url'"})", "application/json; charset=utf-8");
            return;
        }
        QUrl qurl(url);
        if (!qurl.isValid() || (qurl.scheme() != QStringLiteral("http")
                                && qurl.scheme() != QStringLiteral("https"))) {
            res.status = 400;
            res.set_content(R"({"error":"url must be http/https"})", "application/json; charset=utf-8");
            return;
        }
        // method：GET（默认）/ POST / PUT / PATCH / DELETE / HEAD。
        QString method = obj.value(QStringLiteral("method")).toString(QStringLiteral("GET"))
                             .trimmed().toUpper();
        if (method != QStringLiteral("GET") && method != QStringLiteral("POST")
            && method != QStringLiteral("PUT") && method != QStringLiteral("PATCH")
            && method != QStringLiteral("DELETE") && method != QStringLiteral("HEAD")) {
            res.status = 400;
            res.set_content(R"({"error":"method must be GET/POST/PUT/PATCH/DELETE/HEAD"})",
                            "application/json; charset=utf-8");
            return;
        }
        // SSRF 防护：与 /render 同一判定（fetch 是比渲染更直接的 SSRF 载体，绝不能豁免）。
        if (m_ssrfGuardEnabled) {
            std::string ssrfReason = urlSsrfCheck(qurl);
            if (!ssrfReason.empty()) {
                m_queue->anomalyLog().recordRejected(
                    url, QStringLiteral("ssrf_blocked"), QString::fromStdString(ssrfReason));
                res.status = 400;
                std::string body = R"({"error":"url blocked by SSRF guard","detail":")"
                                 + escJson(ssrfReason) + R"("})";
                res.set_content(body, "application/json; charset=utf-8");
                return;
            }
        }
        int timeoutMs = obj.value(QStringLiteral("timeout_ms")).toInt(15000);
        timeoutMs = std::clamp(timeoutMs, 1000, 60000);
        QString reqBody = obj.value(QStringLiteral("body")).toString();
        // 输出模式：text（默认，JSON 字符串内嵌，二进制会经 UTF-8 有损转换）/ base64
        //（二进制安全，>50MB 拒绝）/ file（落盘到 data-dir/fetch/，返回 saved_path）。
        QString responseFormat = obj.value(QStringLiteral("response_format"))
                                     .toString(QStringLiteral("text")).trimmed().toLower();
        if (responseFormat != QStringLiteral("text")
            && responseFormat != QStringLiteral("base64")
            && responseFormat != QStringLiteral("file")) {
            res.status = 400;
            res.set_content(R"({"error":"response_format must be text/base64/file"})",
                            "application/json; charset=utf-8");
            return;
        }
        QString saveFileName = obj.value(QStringLiteral("filename")).toString().trimmed();

        // —— 构造请求（headers 顺序：默认 UA → 调用方头 → 登录 Cookie；worker 逐条
        //    setRawHeader，同名后写覆盖先写，语义与原实现一致）——
        FetchRequest freq;
        freq.url = qurl;
        freq.method = method;
        freq.timeoutMsec = timeoutMs;
        freq.body = reqBody.toUtf8();
        freq.headers.emplace_back(QByteArrayLiteral("User-Agent"),
                                  QByteArray(StealthProfile::userAgent()));
        bool callerSetCookie = false;
        bool callerSetContentType = false;
        {
            QJsonValue hv = obj.value(QStringLiteral("headers"));
            if (hv.isObject()) {
                // 注意必须先落局部变量再迭代：toObject() 返回临时对象，
                // 直接 constBegin/constEnd 各自绑定不同临时（且语句结束即销毁），
                // it.value() 解引用悬垂内存 → SIGSEGV（回测 L3 实撞）。
                const QJsonObject headersObj = hv.toObject();
                for (auto it = headersObj.constBegin(); it != headersObj.constEnd(); ++it) {
                    QByteArray name = it.key().toUtf8();
                    QByteArray value = it.value().toString().toUtf8();
                    // Content-Length/Host/Connection 由 Qt 网络栈自管，调用方设置只会打架。
                    QByteArray lname = name.toLower();
                    if (lname == QByteArrayLiteral("content-length")
                        || lname == QByteArrayLiteral("host")
                        || lname == QByteArrayLiteral("connection")) {
                        continue;
                    }
                    if (lname == QByteArrayLiteral("cookie")) callerSetCookie = true;
                    if (lname == QByteArrayLiteral("content-type")) callerSetContentType = true;
                    freq.headers.emplace_back(name, value);
                }
            }
        }
        // 登录态注入：CookieStore 里该域 cookie 自动携带（调用方显式给了 Cookie 头则尊重调用方）。
        int cookiesApplied = 0;
        if (!callerSetCookie && m_cookies) {
            const auto cookies = m_cookies->cookiesForHost(
                qurl.host(), qurl.scheme() == QStringLiteral("https"));
            QString cookieHeader;
            for (const auto& c : cookies) {
                QNetworkCookie qc(QByteArray::fromStdString(c.name),
                                  QByteArray::fromStdString(c.value));
                if (!cookieHeader.isEmpty()) cookieHeader += QLatin1String("; ");
                cookieHeader += QString::fromUtf8(qc.toRawForm(QNetworkCookie::NameAndValueOnly));
                ++cookiesApplied;
            }
            if (!cookieHeader.isEmpty()) {
                freq.headers.emplace_back(QByteArrayLiteral("Cookie"), cookieHeader.toUtf8());
            }
        }
        // 带 body 的方法：默认 application/json（agent JSON-first 场景），可覆盖。
        const bool hasBody = (method == QStringLiteral("POST") || method == QStringLiteral("PUT")
                              || method == QStringLiteral("PATCH")) && !freq.body.isEmpty();
        if (hasBody && !callerSetContentType) {
            freq.headers.emplace_back(QByteArrayLiteral("Content-Type"),
                                      QByteArrayLiteral("application/json"));
        }

        // —— 同步执行（FetchService 专职线程：长命 QNAM + 事件循环；httplib 线程仅
        //    条件变量等待。取代早期「栈上 QNAM+QEventLoop」——大响应体场景稳定崩溃，
        //    见 FetchService.h 头注释）——
        const qint64 startedMsec = nowMsec();
        const FetchResult fr = m_fetch->execute(freq);
        const qint64 elapsedMs = nowMsec() - startedMsec;
        const QString host = qurl.host().toLower();
        if (fr.timedOut) {
            m_queue->metrics().record(host.toStdString(), false, elapsedMs, 0, 0, false);
            qWarning("[fetch] url=%s method=%s TIMEOUT after %dms",
                     url.left(160).toUtf8().constData(), method.toUtf8().constData(), timeoutMs);
            res.status = 504;
            res.set_content(R"({"error":"fetch timed out"})", "application/json; charset=utf-8");
            return;
        }
        if (!fr.ok) {
            m_queue->metrics().record(host.toStdString(), false, elapsedMs, 0, 0, false);
            qWarning("[fetch] url=%s method=%s FAILED: %s",
                     url.left(160).toUtf8().constData(), method.toUtf8().constData(),
                     fr.error.toUtf8().constData());
            res.status = 502;
            std::string body = R"({"error":"fetch failed","detail":")"
                             + escJson(fr.error.toStdString()) + R"("})";
            res.set_content(body, "application/json; charset=utf-8");
            return;
        }

        // —— 成功：按输出模式组装响应 ——
        m_queue->metrics().record(host.toStdString(), true, elapsedMs, 0, 0, false);
        qWarning("[fetch] url=%s method=%s -> %d (%zuB, %lldms, %s)",
                 url.left(160).toUtf8().constData(), method.toUtf8().constData(), fr.statusCode,
                 static_cast<size_t>(fr.body.size()), elapsedMs, responseFormat.toUtf8().constData());

        std::string out = "{\"url\":\"" + esc(url) +
                          "\",\"final_url\":\"" + esc(fr.finalUrl) +
                          "\",\"status_code\":" + std::to_string(fr.statusCode) +
                          ",\"elapsed_ms\":" + std::to_string(elapsedMs) +
                          ",\"cookies_applied\":" + std::to_string(cookiesApplied);
        // 响应头：同名多值（如 Set-Cookie）逗号拼接。回显服务端会话 cookie 有泄漏面，跳过。
        {
            std::string headersJson;
            QString lastKey;
            for (const auto& h : fr.responseHeaders) {
                const QString key = QString::fromUtf8(h.first).toLower();
                if (key == QLatin1String("set-cookie")) continue;
                const QString value = QString::fromUtf8(h.second);
                if (value.isEmpty()) continue;
                if (key == lastKey && !headersJson.empty()) {
                    // 同名头追加（QNAM 按收到顺序平铺，同名相邻；极端乱序时退化为独立键，
                    // 后写覆盖——与旧行为一致，可接受）。
                    headersJson += ",\"" + esc(key) + "\":\"" + esc(value) + "\"";
                } else {
                    if (!headersJson.empty()) headersJson += ",";
                    headersJson += "\"" + esc(key) + "\":\"" + esc(value) + "\"";
                }
                lastKey = key;
            }
            out += ",\"headers\":{" + headersJson + "}";
        }

        if (responseFormat == QStringLiteral("base64")) {
            // 二进制安全输出：超限（50MB）明确拒绝——base64 会把体积再撑大 1/3，
            // 塞进 JSON 对调用方与传输都不友好，超大文件请用 file 模式落盘。
            static constexpr qsizetype kFetchMaxBase64Bytes = 50 * 1024 * 1024;
            if (fr.body.size() > kFetchMaxBase64Bytes) {
                res.status = 413;
                std::string body = R"({"error":"body too large for base64 output","max_bytes":)"
                                 + std::to_string(int(kFetchMaxBase64Bytes))
                                 + ",\"actual_bytes\":" + std::to_string(int(fr.body.size()))
                                 + R"(,"hint":"use response_format=file to save to disk"})";
                res.set_content(body, "application/json; charset=utf-8");
                return;
            }
            out += ",\"body_bytes\":" + std::to_string(int(fr.body.size()));
            out += ",\"body_base64\":\"" + fr.body.toBase64().toStdString() + "\"}";
            res.set_content(out, "application/json; charset=utf-8");
            return;
        }
        if (responseFormat == QStringLiteral("file")) {
            if (m_fetchSaveDir.isEmpty()) {
                res.status = 500;
                res.set_content(R"({"error":"file output not configured: no save dir"})",
                                "application/json; charset=utf-8");
                return;
            }
            // 文件名：调用方 filename 或 URL 末段，净化为 basename（仅字母数字._-，防路径
            // 穿越/控制符），冲突时加序号后缀；默认兜底 fetch-<epochms>。
            QString fname = saveFileName;
            if (fname.isEmpty()) {
                fname = qurl.path().section(QLatin1Char('/'), -1);
            }
            QString safe;
            for (const QChar ch : fname) {
                const ushort u = ch.unicode();
                const bool okChar = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                                    || (u >= '0' && u <= '9') || ch == QLatin1Char('.')
                                    || ch == QLatin1Char('-') || ch == QLatin1Char('_');
                if (okChar) safe += ch;
            }
            if (safe.isEmpty() || safe == QLatin1String(".")
                || safe == QLatin1String("..")) {
                safe = QStringLiteral("fetch-%1").arg(startedMsec);
            }
            safe = safe.left(120);
            QDir saveDir(m_fetchSaveDir);
            saveDir.mkpath(QStringLiteral("."));
            QString target = saveDir.filePath(safe);
            for (int i = 1; QFileInfo::exists(target); ++i) {
                target = saveDir.filePath(QStringLiteral("%1-%2").arg(safe).arg(i));
                if (i > 999) {   // 防极端冲突死循环
                    target = saveDir.filePath(
                        QStringLiteral("fetch-%1-%2").arg(startedMsec).arg(i));
                    break;
                }
            }
            QFile f(target);
            if (!f.open(QIODevice::WriteOnly) || f.write(fr.body) != fr.body.size()) {
                res.status = 500;
                std::string body = R"({"error":"failed to save file","detail":")"
                                 + escJson(f.errorString().toStdString()) + R"("})";
                res.set_content(body, "application/json; charset=utf-8");
                return;
            }
            out += ",\"saved\":true,\"saved_path\":\"" + esc(QDir::toNativeSeparators(target))
                 + "\",\"bytes\":" + std::to_string(int(fr.body.size())) + "}";
            res.set_content(out, "application/json; charset=utf-8");
            return;
        }

        // text（默认）：10MB 截断（JSON 字符串内嵌；二进制经 UTF-8 有损转换，二进制场景
        // 请用 base64/file）。
        static constexpr qsizetype kFetchMaxBodyBytes = 10 * 1024 * 1024;
        const bool truncated = fr.body.size() > kFetchMaxBodyBytes;
        QByteArray textBody = fr.body;
        if (truncated) textBody.truncate(kFetchMaxBodyBytes);
        out += ",\"body_truncated\":" + std::string(truncated ? "true" : "false");
        out += ",\"body\":\"" + esc(QString::fromUtf8(textBody)) + "\"}";
        res.set_content(out, "application/json; charset=utf-8");
    });

    // 非阻塞状态查询（不含 html/markdown，只状态 + 计时）
    m_srv->Get(R"(/status/([A-Za-z0-9]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        QString id = QString::fromStdString(req.matches[1]);
        RenderQueue::Snapshot s = m_queue->snapshot(id);
        if (!s.found) {
            res.status = 404;
            res.set_content(R"({"error":"task not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(jsonStateResp(s, 0), "application/json; charset=utf-8");
    });

    // 长轮询拉取渲染结果。output=html,markdown 控制返回内容；image 走 /screenshot。
    m_srv->Get(R"(/result/([A-Za-z0-9]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        QString id = QString::fromStdString(req.matches[1]);
        OutputMask outMask = static_cast<OutputMask>(Output::Html);
        if (req.has_param("output")) {
            outMask = parseOutputs(QJsonValue(QString::fromStdString(req.get_param_value("output"))));
        }
        RenderQueue::Snapshot s = m_queue->snapshot(id);
        if (!s.found) {
            res.status = 404;
            res.set_content(R"({"error":"task not found"})", "application/json; charset=utf-8");
            return;
        }
        if (s.done) {
            res.set_content(jsonStateResp(s, outMask), "application/json; charset=utf-8");
            return;
        }
        // 默认长轮询 25s
        unsigned long timeoutMs = 25000UL;
        if (req.has_param("timeout")) {
            // strtoul 容错：?timeout=abc 不抛异常（关异常编译会崩）。errno 检测溢出。
            errno = 0;
            const std::string& ts = req.get_param_value("timeout");
            char* end = nullptr;
            unsigned long t = std::strtoul(ts.c_str(), &end, 10);
            // end 指向首个非数字字符；全空串或含非数字 → end==ts.c_str() → 视为非法，用默认值。
            if (end != ts.c_str() && errno == 0 && t > 0) {
                timeoutMs = std::min<unsigned long>(t, 60000UL);
            }
        }
        m_queue->waitForCompletion(id, timeoutMs);  // 阻塞等待终态/超时
        s = m_queue->snapshot(id);                   // 等待后再取一次安全快照
        if (!s.found) {
            res.status = 404;
            res.set_content(R"({"error":"task not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(jsonStateResp(s, s.done ? outMask : 0), "application/json; charset=utf-8");
    });

    // 下载 HAR 网络录制（仅当任务请求了 record_network=true 且渲染时录制了网络）。
    // 产物是标准 HAR 1.2 JSON（含 Chrome 扩展字段 _resourceType/_initiator/_transferSize），
    // 可被 har-validator 校验、Chrome DevTools / Charles / haralyzer 导入分析。
    m_srv->Get(R"(/har/([A-Za-z0-9]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        if (!m_harStore) {
            res.status = 503;
            // 给出可操作的开启指引（供 agent / 运维自助修复），而非仅说 disabled。
            QJsonObject ej;
            ej["error"] = QStringLiteral("HAR recording disabled");
            ej["reason"] = QStringLiteral("server not started with --remote-debugging-port");
            ej["hint"] = QStringLiteral("Restart seimi-render with --remote-debugging-port <port> "
                                        "to enable CDP-based network recording. "
                                        "Note: this skips stealth's AutomationControlled flag "
                                        "(debug mode and anti-bot are mutually exclusive).");
            ej["example"] = QStringLiteral("seimi-render --remote-debugging-port 9222 "
                                           "--http-port 8088 --ws-port 8089");
            res.set_content(QString::fromUtf8(QJsonDocument(ej).toJson(QJsonDocument::Compact)).toStdString(),
                            "application/json; charset=utf-8");
            return;
        }
        QString id = QString::fromStdString(req.matches[1]);
        HarStore::Result r = m_harStore->get(id);
        if (!r.found) {
            res.status = 404;
            res.set_content(R"({"error":"HAR not found"})",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(r.harJson.toStdString(), "application/json; charset=utf-8");
        res.set_header("Content-Disposition",
                       "attachment; filename=\"" + id.toStdString() + ".har\"");
    });

    // 下载打印 PDF（仅当任务请求了 pdf 且渲染成功）
    m_srv->Get(R"(/pdf/([A-Za-z0-9]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        QString id = QString::fromStdString(req.matches[1]);
        RenderQueue::Snapshot s = m_queue->snapshot(id);
        if (!s.found) {
            res.status = 404;
            res.set_content(R"({"error":"task not found"})", "application/json; charset=utf-8");
            return;
        }
        if (!s.done) {
            res.status = 409;
            res.set_content(R"({"error":"task not finished"})", "application/json; charset=utf-8");
            return;
        }
        if (s.pdfData.isEmpty()) {
            res.status = 404;
            res.set_content(R"({"error":"no pdf for this task; request output=pdf when submitting"})",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(s.pdfData.toStdString(), "application/pdf");
    });

    // 下载 PNG 截图（仅当任务请求了 screenshot 且渲染成功）。
    // 与 /pdf/:id 平行：二进制直接返回，避免 base64 撑大 JSON。
    m_srv->Get(R"(/image/([A-Za-z0-9]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
        SEIMI_AUTH(req, res);
        QString id = QString::fromStdString(req.matches[1]);
        RenderQueue::Snapshot s = m_queue->snapshot(id);
        if (!s.found) {
            res.status = 404;
            res.set_content(R"({"error":"task not found"})", "application/json; charset=utf-8");
            return;
        }
        if (!s.done) {
            res.status = 409;
            res.set_content(R"({"error":"task not finished"})", "application/json; charset=utf-8");
            return;
        }
        if (s.imageData.isEmpty()) {
            res.status = 404;
            res.set_content(R"({"error":"no screenshot for this task; request output=screenshot when submitting"})",
                            "application/json; charset=utf-8");
            return;
        }
        // Content-Type 按实际编码格式（智能选择或手动指定的结果）。
        const char* ct = (s.resolvedImageFmt == ImageFormat::Jpeg) ? "image/jpeg" : "image/png";
        res.set_content(s.imageData.toStdString(), ct);
    });
}

} // namespace seimi
