// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

// seimi-render: Qt WebEngine 渲染服务。
// 提交 URL -> WebEngine 渲染 -> 返回 HTML/Markdown/PDF/截图，支持长轮询与 WebSocket 推送。
//
// 线程拓扑：
//   主线程(GUI): QApplication 事件循环
//                ├─ RenderPool  (N 个 WebEngine page，并发渲染)
//                ├─ WsServer    (WebSocket 推送，信号槽)
//                └─ HttpServer 跑在独立 std::thread，通过线程安全 RenderQueue 交互
// 启动先起 QApplication 让 Chromium 子进程就绪，再起渲染池/HTTP/WS；退出反向关闭。

#include "HttpServer.h"
#include "McpServer.h"
#include "CookieStore.h"
#include "Environment.h"
#include "ProxyConfig.h"
#include "RenderPool.h"
#include "RenderQueue.h"
#include "WsServer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QMessageLogContext>
#include <QTimer>
#include <QWebEngineUrlScheme>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifdef __unix__
#include <unistd.h>
#endif

namespace seimi {

// Qt 消息处理器：降噪。被渲染的重 SPA（gemini/doubao 等）会刷大量噪声——
// Chromium 转发的网页 console（"js:" 前缀，CSP/废弃 API 警告等）、Chromium 安全告警
//（"Mixed Content:" 等）、网页 JS 未捕获错误（裸 "Error:" 行）全是网页/Chromium 背景噪声，
// 与渲染正确性无关。默认静音；--verbose-chromium 时全量透传。
// 用 Qt 消息处理器（而非 --log-level）是因为这些转发都走 Qt 日志系统，--log-level 管不到。
static bool g_verboseChromium = false;
static void noiseFilter(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    const QByteArray raw = msg.toUtf8();
    // 静音模式下，丢弃已知的网页/Chromium 噪声（按特征匹配）。
    if (!g_verboseChromium) {
        const char* s = raw.constData();
        // Chromium 内部噪声（无前缀，来自浏览器自身，非网页 console）
        if (strstr(s, "Path override failed for key base::DIR_APP_DICTIONARIES") != nullptr
            || strstr(s, "Dropped Escape call") != nullptr
            // Chromium 安全告警：HTTPS 页加载 HTTP 资源等。带 blog.chromium.org 官方链接，
            // 只可能由 Chromium 产生，绝不来自 seimi-render 自身逻辑。
            || strstr(s, "Mixed Content:") != nullptr
            // Chromium DevTools 协议/扩展相关背景噪声
            || strstr(s, "DevTools listening on") != nullptr) {
            return;
        }
        // 网页 console / JS 未捕获错误：Chromium 转发的网页侧输出。
        //   - "js: " 前缀：console.log/warn/error 等
        //   - "Mixed Content:": 见上
        //   - 裸 "Error:" / "Uncaught" 开头：网页 JS 抛出未捕获异常（如被限流站点抛
        //     "Error: rate limit exceeded"）。必须 startsWith 而非 strstr——避免误杀
        //     seimi-render 自身含 "error" 字样的真实诊断（如 "page load failed: ..."）。
        if (msg.startsWith(QLatin1String("js: "))
            || msg.startsWith(QLatin1String("Error: "))
            || msg.startsWith(QLatin1String("Uncaught "))) {
            return;
        }
    }
    // 其余消息（含 seimi-render 自己的 stdout/stderr 输出、真正的 ERROR/FATAL）照常打印。
    fprintf(stderr, "%s\n", raw.constData());
}

struct Config {
    std::string host = "127.0.0.1"; // 默认仅本机回环；需对外暴露时显式 --host 0.0.0.0
    bool hostExplicit = false;      // 用户是否显式给定了 --host（决定 MCP 强制回环）
    int httpPort = 8088;
    int wsPort = 8089;
    int mcpPort = 8090;        // MCP (Model Context Protocol) HTTP 端口
    int concurrency = 3;          // WebEngine page 实例数（并发渲染上限）
    int httpThreads = 8;          // httplib 线程池大小
    int settleDefaultMs = 2000;   // 默认 loadFinished 后等待 JS 执行
    qint64 loadTimeoutMs = 30000; // 单任务加载总超时
    bool windowed = false;        // 强制原生窗口平台（调试用）；默认 offscreen
    bool noSandbox = false;       // Chromium --no-sandbox（WSL2/容器/root 必需）
    bool sandboxExplicit = false; // --sandbox 显式给定（用于覆盖 root 自动判定）
    bool verboseChromium = false; // --verbose-chromium：调低 Chromium 日志级别看详细日志
    std::string adminPassword;    // --password / --password-file / SEIMI_PASSWORD（空=不启用密码保护）
    std::string passwordFile;     // --password-file <path>：从文件读首行作密码（避免进 ps 命令行）
    bool adminEnabled = true;     // --no-admin：关闭内置管理界面
    std::string trustedProxies;   // --trusted-proxy <list>：可信反代网段（精确 IP 或 CIDR，逗号分隔），
                                   // 非空时 /api/login 按 X-Forwarded-For 还原真实客户端 IP 限流。
    // 网络代理：经 QNetworkProxy::setApplicationProxy 运行时动态配置（见 ProxyConfig）。
    // 初始代理可空（直连），运行时由 POST /proxy 动态更新，无需重启。
    ProxySpec initialProxy;       // 启动时 --proxy 指定的初始代理（默认 Direct 直连）
    bool stealthEnabled = true;   // 浏览器指纹统一（默认开启，--no-stealth 关闭）
    bool warmupEnabled = true;    // Google 会话预热（默认开启，--no-warmup 关闭）
    std::string warmupUrl = "https://www.google.com/";  // --warmup-url 自定义预热目标
};

static void printUsage() {
    std::fprintf(stderr,
        "Usage: seimi-render [options]\n"
        "  --http-port <n>       HTTP port (default 8088)\n"
        "  --ws-port <n>         WebSocket port (default 8089)\n"
        "  --mcp-port <n>        MCP (Model Context Protocol) HTTP port (default 8090)\n"
        "  --host <addr>         bind host (default 127.0.0.1; use 0.0.0.0 to expose)\n"
        "  --concurrency <n>     WebEngine render slots (default 3)\n"
        "  --http-threads <n>    HTTP worker threads (default 8)\n"
        "  --settle-ms <n>       default JS settle delay after load (default 2000)\n"
        "  --load-timeout-ms <n> per-task load timeout (default 20000)\n"
        "  --windowed            force native windowed QPA platform (default: offscreen for headless service)\n"
        "  --no-sandbox          disable Chromium sandbox (WSL2/container/root often need this)\n"
        "  --sandbox             force Chromium sandbox on (overrides auto-detect when running as root)\n"
        "  --verbose-chromium    show Chromium/web console logs (default: known web noise filtered out)\n"
        "  --password <pw>       admin password (INSECURE: visible in `ps`/process list)\n"
        "  --password-file <f>   read password from first line of <f> (recommended over --password)\n"
        "  SEIMI_PASSWORD env    password from environment variable (recommended)\n"
        "                        Password precedence: --password > --password-file > SEIMI_PASSWORD.\n"
        "                        Empty (none given) = no password, open access.\n"
        "  --no-admin            disable built-in admin UI (default: enabled at GET /)\n"
        "  --trusted-proxy <list> comma-separated trusted reverse-proxy IPs/CIDRs\n"
        "                        (e.g. 10.8.0.0/16,127.0.0.1). When set, /api/login rate-\n"
        "                        limits by the real client IP extracted from X-Forwarded-For;\n"
        "                        otherwise the TCP peer address is used (XFF ignored, forge-safe).\n"
        "  --proxy <url>         upstream proxy for all Chromium traffic. Format:\n"
        "                          http://[user:pass@]host:port\n"
        "                          socks5://[user:pass@]host:port\n"
        "                        Set via QNetworkProxy::setApplicationProxy so it can be\n"
        "                        hot-swapped at runtime via POST /proxy (no restart needed).\n"
        "                        Omit for direct connection; type=direct clears it.\n"
        "  --no-stealth          disable browser fingerprint unification (default: enabled).\n"
        "                        Stealth unifies all render instances into a single Chrome\n"
        "                        desktop fingerprint (UA/screen/WebGL/canvas) to blend into\n"
        "                        the crowd and bypass basic anti-bot detection (e.g. Google).\n"
        "  --no-warmup           disable Google session warm-up at startup (default: enabled).\n"
        "                        Warm-up renders --warmup-url once to acquire Google cookies\n"
        "                        (NID/SOCS) before dispatching render tasks; re-warms every 30min.\n"
        "  --warmup-url <url>    warm-up target (default https://www.google.com/; must be http/https)\n"
        "  --help                show this help\n");
}

// 从文件首行读取密码（去掉结尾的 \r\n）。失败返回空。
static std::string readPasswordFile(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return std::string();
    QString line = QString::fromUtf8(f.readLine());
    // 去掉行尾换行（跨平台）
    while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
    return line.toStdString();
}

// 解析代理 URL 为 ProxySpec。
// 接受 "http://[user:pass@]host:port" 或 "socks5://[user:pass@]host:port"。
// scheme 缺省视为 http。解析失败（无 host/port）返回 Direct。
static ProxySpec parseProxyUrl(const std::string& s) {
    ProxySpec spec;
    QString url = QString::fromStdString(s).trimmed();
    if (url.isEmpty()) return spec;  // Direct
    int schemeEnd = url.indexOf(QStringLiteral("://"));
    QString scheme;
    QString rest;
    if (schemeEnd > 0) {
        scheme = url.left(schemeEnd).toLower();
        rest = url.mid(schemeEnd + 3);
    } else {
        scheme = QStringLiteral("http");
        rest = url;
    }
    if (scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks")) {
        spec.type = ProxySpec::Type::Socks5;
    } else {
        spec.type = ProxySpec::Type::Http;
    }
    // 剥离 user:pass@
    int at = rest.lastIndexOf(QLatin1Char('@'));
    QString credPart;
    if (at >= 0) {
        credPart = rest.left(at);
        rest = rest.mid(at + 1);
        int colon = credPart.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            spec.user = credPart.left(colon).toStdString();
            spec.pass = credPart.mid(colon + 1).toStdString();
        } else {
            spec.user = credPart.toStdString();
        }
    }
    // host[:port]
    int colon = rest.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        spec.host = rest.left(colon).toStdString();
        spec.port = quint16(rest.mid(colon + 1).toUShort());
    } else {
        spec.host = rest.toStdString();
    }
    if (spec.host.empty() || spec.port == 0) {
        // 无有效 host/port，退化为直连（启动期容错，不致命）。
        spec.type = ProxySpec::Type::Direct;
        spec.host.clear();
        spec.port = 0;
    }
    return spec;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int& out) {
            if (i + 1 < argc) { out = std::atoi(argv[++i]); }
        };
        if (a == "--help" || a == "-h") { printUsage(); std::exit(0); }
        else if (a == "--http-port" && i + 1 < argc) c.httpPort = std::atoi(argv[++i]);
        else if (a == "--ws-port" && i + 1 < argc)   c.wsPort = std::atoi(argv[++i]);
        else if (a == "--mcp-port" && i + 1 < argc)  c.mcpPort = std::atoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) { c.host = argv[++i]; c.hostExplicit = true; }
        else if (a == "--concurrency") next(c.concurrency);
        else if (a == "--http-threads") next(c.httpThreads);
        else if (a == "--settle-ms") next(c.settleDefaultMs);
        else if (a == "--load-timeout-ms" && i + 1 < argc) c.loadTimeoutMs = std::atoll(argv[++i]);
        else if (a == "--windowed") c.windowed = true;
        else if (a == "--no-sandbox") { c.noSandbox = true; c.sandboxExplicit = true; }
        else if (a == "--sandbox")    { c.noSandbox = false; c.sandboxExplicit = true; }
        else if (a == "--verbose-chromium") c.verboseChromium = true;
        else if (a == "--password" && i + 1 < argc) c.adminPassword = argv[++i];
        else if (a == "--password-file" && i + 1 < argc) c.passwordFile = argv[++i];
        else if (a == "--no-admin")   c.adminEnabled = false;
        else if (a == "--trusted-proxy" && i + 1 < argc) c.trustedProxies = argv[++i];
        else if (a == "--proxy" && i + 1 < argc) c.initialProxy = parseProxyUrl(argv[++i]);
        else if (a == "--no-stealth") c.stealthEnabled = false;
        else if (a == "--no-warmup") c.warmupEnabled = false;
        else if (a == "--warmup-url" && i + 1 < argc) c.warmupUrl = argv[++i];
    }
    c.concurrency   = std::max(1, c.concurrency);
    c.httpThreads   = std::max(2, c.httpThreads);
    c.settleDefaultMs = std::max(0, c.settleDefaultMs);
    c.loadTimeoutMs = std::max(1000LL, c.loadTimeoutMs);

    // 密码来源合并：优先级 --password > --password-file > SEIMI_PASSWORD。
    // 后两者避免密码出现在进程命令行（ps 可见），推荐使用。
    if (c.adminPassword.empty() && !c.passwordFile.empty()) {
        c.adminPassword = readPasswordFile(c.passwordFile);
    }
    if (c.adminPassword.empty()) {
        QByteArray env = qgetenv("SEIMI_PASSWORD");
        if (!env.isEmpty()) c.adminPassword = env.toStdString();
    }
    return c;
}

} // namespace seimi

int main(int argc, char** argv) {
    using namespace seimi;

    Config cfg = parseArgs(argc, argv);
    g_verboseChromium = cfg.verboseChromium;
    qInstallMessageHandler(noiseFilter);  // 安装降噪过滤器（在 QApplication 之前）

    // warmup URL 校验：仅 http/https（运维配置项，不过 SSRF——同 --proxy 语义）。
    if (cfg.warmupEnabled
        && cfg.warmupUrl.rfind("http://", 0) != 0
        && cfg.warmupUrl.rfind("https://", 0) != 0) {
        std::fprintf(stderr, "FATAL: --warmup-url must be http/https: %s\n", cfg.warmupUrl.c_str());
        return 1;
    }

    // 平台选择：无头渲染服务默认 offscreen（无窗口、不依赖窗口系统、事件循环稳定）。
    // --windowed 强制原生平台（调试看真实窗口）。
    if (!cfg.windowed) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // Chromium sandbox：WSL2/容器/root 运行时用户命名空间 sandbox 常启动失败，需 --no-sandbox。
    // 优先级：1) 显式 --no-sandbox/--sandbox 听用户；2) 未指定且以 root 运行则自动启用并提示。
    if (!cfg.sandboxExplicit) {
#ifdef __unix__
        if (getuid() == 0) {
            cfg.noSandbox = true;
            std::fprintf(stderr,
                "NOTE: running as root — auto-enabling Chromium --no-sandbox "
                "(use --sandbox to override).\n");
        }
#endif
    }
    if (cfg.noSandbox) {
        QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        if (existing.indexOf("--no-sandbox") < 0) {
            QByteArray flags = existing.trimmed();
            if (!flags.isEmpty()) flags += ' ';
            flags += "--no-sandbox";
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
        }
    }

    // 网络代理：经 QNetworkProxy::setApplicationProxy 运行时动态配置（见 ProxyConfig）。
    // Qt WebEngine 轮询全局应用代理，运行时修改后 Chromium 在数秒内切换。具体 apply 由 RenderPool GUI 线程定时器驱动。


    // offscreen 模式下的软件渲染（Linux + macOS，解决无头截图白屏）：
    // Chromium 默认硬件 GPU 合成，结果落在独立 GPU 进程的 surface，不回写 QWidget 的 CPU
    // backing store，而 view->grab() 读的正是 backing store → 截图空白。
    // 注入 --disable-gpu 强制软件合成，帧光栅化进 backing store，grab() 才能取到真实像素。
    // 平台差异：Linux/WSL/容器无 GPU 或驱动不全，必须软件渲染；macOS Metal 合成同样不落
    // backing store，需软件渲染；Windows DirectX offscreen 合成会回读 backing store，截图正常，不注入。
    // 用户可通过 QTWEBENGINE_CHROMIUM_FLAGS 覆盖（已含相关 flag 则不重复注入）。
    if (!cfg.windowed) {
        QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
#if defined(__linux__) || defined(__APPLE__)
        if (existing.indexOf("--disable-gpu") < 0
            && existing.indexOf("--use-gl=") < 0
            && existing.indexOf("--use-angle=") < 0) {
            QByteArray flags = existing.trimmed();
            if (!flags.isEmpty()) flags += ' ';
            flags += "--disable-gpu";
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
            std::fprintf(stderr,
                "NOTE: offscreen mode — disabling Chromium GPU "
                "(--disable-gpu) for headless software rendering / screenshot support.\n");
        }
#endif
    }

    // 浏览器指纹统一（stealth）：注入 Chromium flag 在原生层抹除自动化标记。
    // AutomationControlled 控制 navigator.webdriver 与 CDP 痕迹，默认开启会被反爬识别。
    // 同时禁用 Translate/OptimizationHints/MediaRouter 减少额外网络请求指纹面。
    if (cfg.stealthEnabled) {
        QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        QByteArray flags = existing.trimmed();

        // 核心：禁用 AutomationControlled（抹除 webdriver + CDP 痕迹）。
        if (existing.indexOf("AutomationControlled") < 0) {
            if (!flags.isEmpty()) flags += ' ';
            flags += "--disable-blink-features=AutomationControlled";
        }

        // UA 统一：--user-agent flag / setHttpUserAgent / JS Worker 包装三条路径均无法覆盖
        // Worker context UA（QtWebEngine 已知残留），文档层 UA 已统一 142，详见 stealth.js 注释。

        // 禁用产生额外网络请求/发现协议的 features，减少指纹面：
        // Translate（翻译弹窗+语言检测请求）、OptimizationHints（向 Google 取优化提示）、
        // MediaRouter/DialMediaRouteProvider（局域网媒体设备发现，暴露网络环境）。
        if (existing.indexOf("--disable-features") < 0) {
            if (!flags.isEmpty()) flags += ' ';
            flags += "--disable-features=Translate,OptimizationHints,MediaRouter,DialMediaRouteProvider";
        }

        if (flags != existing) {
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
        }
    }


    // WebEngine 需要在 GUI 线程运行；必须用 QApplication（而非 QCoreApplication）。
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);

    // 渲染队列：全局线程安全中枢。
    RenderQueue queue;

    // Cookie 同步中枢：浏览器插件通过 POST /cookies 灌入登录态，
    // 由 RenderPool 在 GUI 线程周期 apply 到共享 profile。
    // 传入 data/ 目录启用加密持久化（重启自动恢复登录态）。
    CookieStore cookies(QCoreApplication::applicationDirPath() + "/data");

    // 网络代理配置中枢（GUI 线程 apply）：HTTP 线程经 POST /proxy 写入，
    // 由 RenderPool 的 GUI 线程定时器周期调 apply() 把新代理写入
    // QNetworkProxy::setApplicationProxy（Qt WebEngine 轮询它，运行时动态生效）。
    ProxyConfig proxyConfig;
    if (cfg.initialProxy.type != ProxySpec::Type::Direct) {
        proxyConfig.setProxy(cfg.initialProxy);
    }

    // 渲染池（GUI 线程）：N 个 WebEngine 并发。
    RenderPool pool(&queue, cfg.concurrency, cfg.loadTimeoutMs, &app);
    pool.setCookieStore(&cookies);
    pool.setProxyConfig(&proxyConfig);
    pool.setStealthEnabled(cfg.stealthEnabled);
    pool.setWarmupEnabled(cfg.warmupEnabled);
    pool.setWarmupUrl(QString::fromStdString(cfg.warmupUrl));

    // WebSocket 服务（GUI 线程）：支持 render 请求与 subscribe 订阅。
    // 启用密码时传入同一确定性 token，WS 端口同样受保护（见 WsServer 鉴权）。
    WsServer ws(&queue, cfg.settleDefaultMs, HttpServer::computeToken(cfg.adminPassword), &app);
    // WS 必须与 HTTP 绑同一 --host：默认 127.0.0.1 时只听本机，避免在所有网卡上
    // 暴露无密码的渲染/SSRF 入口。"localhost" 不是合法地址字面量，单独映射到回环。
    QHostAddress wsAddr;
    if (cfg.host == "localhost") {
        wsAddr = QHostAddress(QHostAddress::LocalHost);
    } else if (!wsAddr.setAddress(QString::fromStdString(cfg.host))) {
        wsAddr = QHostAddress::Any;  // 解析失败时回退（与 httplib 对未知 host 的宽容一致）
    }
    if (!ws.listen(wsAddr, quint16(cfg.wsPort))) {
        std::fprintf(stderr, "FATAL: cannot listen WebSocket on port %d\n", cfg.wsPort);
        return 1;
    }

    // 渲染完成 -> 转发到 WS 推送（同在 GUI 线程，直接信号槽）。
    QObject::connect(&pool, &RenderPool::taskFinished,
        [&queue, &ws](const QString& id) {
            // 取最终 state 以推送给订阅者
            RenderTaskPtr t = queue.peek(id);
            QString state = t ? QString::fromLatin1(taskStateName(t->state))
                              : QStringLiteral("failed");
            bool blocked = t && t->blocked;   // 反爬拦截标记随推送透出
            ws.notifyFinished(id, state, blocked);
        });

    // 管理 UI 资源目录定位：优先二进制同级 admin-ui/；开发模式（同级没有）回退到源码目录。
    // 源码目录路径由 CMakeLists 通过 SEIMI_ADMIN_UI_SRC_DIR 宏注入（编译期）。
    QString binDir = QCoreApplication::applicationDirPath();
    QString adminUiDir = binDir + "/admin-ui";
    if (!QFile::exists(adminUiDir + "/index.html")) {
#ifdef SEIMI_ADMIN_UI_SRC_DIR
        QString srcDir = QStringLiteral(SEIMI_ADMIN_UI_SRC_DIR);
        if (QFile::exists(srcDir + "/index.html")) adminUiDir = srcDir;
        else adminUiDir.clear();
#else
        adminUiDir.clear();
#endif
    }
    std::string adminUi = cfg.adminEnabled ? adminUiDir.toStdString() : std::string();

    // 运行环境采集器：构造时采集静态字段（OS/CPU/内存/构建信息），
    // GUI 线程 3s 定时器采样实时 CPU/RSS，供 /status 的 environment 段。
    Environment env(&app);
    env.start(&app);

    // HTTP server 跑在独立线程（httplib::listen 阻塞）。
    HttpServer http(&queue, &cookies, cfg.settleDefaultMs,
                    adminUi, cfg.adminPassword, &app);
    http.setEnvironment(&env);
    // 可信反代网段：非空时 /api/login 按 X-Forwarded-For 还原真实 IP 限流（反代部署用）。
    if (!cfg.trustedProxies.empty()) {
        http.setTrustedProxies(cfg.trustedProxies);
    }
    // 网络代理配置：注册 GET/POST/DELETE /proxy 接口。
    http.setProxyConfig(&proxyConfig);
    std::thread httpThread([&]() {
        if (!http.start(cfg.host, cfg.httpPort, cfg.httpThreads)) {
            std::fprintf(stderr, "FATAL: HTTP server failed to listen on %s:%d\n",
                         cfg.host.c_str(), cfg.httpPort);
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        }
    });

    // MCP server（供 Claude Code 等 agent 工具接入）。工具内部经 HTTP 调本机渲染 API。
    // 启用密码时工具调渲染 API 带同一确定性 token 鉴权。
    // MCP 端口绑定：用户未显式 --host 时强制回环，避免误开公网。需远程 MCP 显式 --host 0.0.0.0。
    std::string mcpHost = cfg.hostExplicit ? cfg.host : std::string("127.0.0.1");
    McpServer mcp;
    mcp.start(mcpHost, cfg.mcpPort, "127.0.0.1", cfg.httpPort,
              HttpServer::computeToken(cfg.adminPassword));

    // 优雅退出：信号到来时按序关闭。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        queue.stop();   // 唤醒渲染消费者 + 长轮询线程
        pool.stop();
        http.stop();    // 让 httplib::listen 返回
        if (httpThread.joinable()) httpThread.join();
        mcp.stop();
        cookies.flush();  // 退出前确保 cookie 持久化落盘
    });

    std::fprintf(stdout,
        "seimi-render ready\n"
        "  HTTP       : http://%s:%d%s\n"
        "  WebSocket  : ws://%s:%d%s\n"
        "  MCP        : http://%s:%d  (for Claude Code / agent tools)%s\n"
        "  Admin UI   : http://%s:%d/  (admin console: test / stats / cookies / mcp / docs)%s\n"
        "  concurrency: %d render slots\n"
        "  http-threads: %d\n",
        cfg.host.c_str(), cfg.httpPort,
            cfg.host == "127.0.0.1" || cfg.host == "localhost" ? "" : "  [EXPOSED]",
        cfg.host.c_str(), cfg.wsPort,
            cfg.host == "127.0.0.1" || cfg.host == "localhost" ? "" : "  [EXPOSED]",
        mcpHost.c_str(), cfg.mcpPort,
            mcpHost == "127.0.0.1" || mcpHost == "localhost" ? "" : "  [EXPOSED]",
        cfg.host.c_str(), cfg.httpPort,
        cfg.adminPassword.empty() ? "  [NO PASSWORD - open access]"
                                  : "  [password protected]",
        cfg.concurrency, cfg.httpThreads);
    // 代理状态提示
    {
        ProxySpec cur = proxyConfig.snapshot();
        const char* typeName = cur.type == ProxySpec::Type::Http ? "http"
                             : cur.type == ProxySpec::Type::Socks5 ? "socks5" : "direct";
        std::fprintf(stdout,
            "  Proxy     : %s%s\n", typeName,
            cur.type == ProxySpec::Type::Direct ? " (direct connection)" : "");
    }
    // 构建版本信息（编译期由 CMake 注入；缺省值兼容项目 CMake 之外的手动编译）。
    //   Version    : 统一版本号（唯一可信源 = CMakeLists.txt 顶部 project(VERSION)，迭代时改那一处）
    //   Build time : 构建时刻（configure-time，UTC+8 东八区）
    //   Git commit : 最新已提交 commit（有未提交改动时也显示最新 commit，dirty 标注工作区状态）
#ifndef SEIMI_VERSION
#define SEIMI_VERSION "unknown"
#endif
#ifndef SEIMI_BUILD_TIME
#define SEIMI_BUILD_TIME "unknown"
#endif
#ifndef SEIMI_GIT_COMMIT
#define SEIMI_GIT_COMMIT "unknown"
#endif
#ifndef SEIMI_GIT_DIRTY
#define SEIMI_GIT_DIRTY "nogit"
#endif
    std::fprintf(stdout,
        "  Version   : %s\n"
        "  Build time: %s\n"
        "  Git commit: %s (%s)\n",
        SEIMI_VERSION, SEIMI_BUILD_TIME, SEIMI_GIT_COMMIT, SEIMI_GIT_DIRTY);
    // 对外暴露且无密码时给显著警告（防误开放匿名 SSRF/cookie 入口）。
    if ((cfg.host != "127.0.0.1" && cfg.host != "localhost") && cfg.adminPassword.empty()) {
        std::fprintf(stderr,
            "WARNING: listening on %s with NO password — anyone on the network can\n"
            "         drive renders, inject cookies, and use this as an SSRF proxy.\n"
            "         Add --password / --password-file / SEIMI_PASSWORD to protect it.\n",
            cfg.host.c_str());
    }
    // 对外暴露（含反代场景）且启用了密码，却没配可信代理：此时登录限流按 TCP 直连地址
    //（通常即反代 IP）走，等于所有客户端共用一份计数，限流退化。反代部署应配
    // --trusted-proxy 让 /api/login 按 X-Forwarded-For 的真实 IP 限流。
    if ((cfg.host != "127.0.0.1" && cfg.host != "localhost")
        && !cfg.adminPassword.empty()
        && cfg.trustedProxies.empty()) {
        std::fprintf(stderr,
            "NOTE: behind a reverse proxy? Set --trusted-proxy <ip/cidr,...> so that\n"
            "      /api/login rate-limits by the real client IP (from X-Forwarded-For).\n"
            "      Without it, all clients share the proxy's address for login throttling.\n");
    }
    std::fflush(stdout);

    // 启动渲染池（必须在事件循环开始前建好 page，由 start() 完成）。
    pool.start();

    // 关键：禁止"最后一个窗口关闭即退出"。截图需 view->show() 激活渲染管线，destroyPage()
    // 时 view 被 deleteLater 关闭——若不禁用此默认行为，首个任务完成后 view 关闭会触发
    // lastWindowClosed → app.quit()。无头服务只响应显式信号。
    app.setQuitOnLastWindowClosed(false);

    int rc = app.exec();
    return rc;
}
