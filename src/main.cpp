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
#include "AppConfig.h"
#include <QDir>
#include "CookieStore.h"
#include "Environment.h"
#include "HarRecorder.h"
#include "HarStore.h"
#include "ProxyConfig.h"
#include "RenderPool.h"
#include "RenderQueue.h"
#include "UpdateManager.h"
#include "WsServer.h"
#include "gui/GuiApp.h"
#include "gui/LogModel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMessageLogContext>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QWebEngineUrlScheme>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef __unix__
#include <unistd.h>
#endif

#ifdef _WIN32
// SetConsoleOutputCP / SetConsoleCP：把控制台代码页切到 UTF-8，避免 UTF-8 日志被
// 当 GBK 解码导致中文乱码（Windows 控制台默认代码页 936）。
// WIN32_LEAN_AND_MEAN + NOMINMAX：精简 windows.h，并禁止 min/max 宏——否则后面的
// std::max（parseArgs 里多处）会被宏展开成语法错误（main.cpp:252 编译失败即此因）。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace seimi {

// config.json 文件态（GUI 设置页的编辑对象；默认构造 = 全默认/未指定）。
// 启动期在 main 里由 AppConfig::load 填充后存入，GuiApp 构造时取走副本。
static AppConfig g_appConfig;
// 实际生效的数据目录（经 config.json data_dir 一跳重定向后的值；GUI 表单展示用）。
static QString g_effectiveDataDir;

// 优雅退出信号桥：SIGINT/SIGTERM / Windows 控制台事件 → QueuedConnection 进事件循环
// → aboutToQuit 关机链（queue.stop → pool.stop → http/mcp 停 → cookie/metrics 落盘）。
// 此前 Ctrl+C 是硬杀，cookies.dat/metrics.json 的退出前 flush 全部丢失。
static QCoreApplication* g_appForQuit = nullptr;
static void requestGracefulQuit() {
    if (!g_appForQuit) return;
    QMetaObject::invokeMethod(g_appForQuit, &QCoreApplication::quit, Qt::QueuedConnection);
}
#ifdef _WIN32
static BOOL WINAPI consoleCtrlHandler(DWORD) { requestGracefulQuit(); return TRUE; }
#else
static void posixSignalHandler(int) { requestGracefulQuit(); }
#endif

// Qt 消息处理器：降噪。被渲染的重 SPA（gemini/doubao 等）会刷大量噪声——
// Chromium 转发的网页 console（"js:" 前缀，CSP/废弃 API 警告等）、Chromium 安全告警
//（"Mixed Content:" 等）、网页 JS 未捕获错误（裁 "Error:" 行）全是网页/Chromium 背景噪声，
// 与渲染正确性无关。默认静音；--verbose-chromium 时全量透传。
// 用 Qt 消息处理器（而非 --log-level）是因为这些转发都走 Qt 日志系统，-log-level 管不到。
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
    LogModel::bridgeAppend(type, QString::fromLatin1(ctx.category), msg); // GUI 日志页（无实例时零成本）
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
    bool verboseChromium = false; // --verbose-chromium：调试 Chromium 日志级别看详细日志
    std::string adminPassword;    // --password / --password-file / SEIMI_PASSWORD（空=不启用密码保护）
    std::string passwordFile;     // --password-file <path>：从文件读首行作密码（避免进 ps 命令行）
    bool adminEnabled = true;     // --no-admin：关闭内置管理界面
    bool ssrfGuardEnabled = true; // --no-ssrf：关闭 SSRF 防护（本机自用 Agent 场景，允许访问内网/回环目标）
    std::string trustedProxies;   // --trusted-proxy <list>：可信反代网段（精确 IP 或 CIDR，逗号分隔），
                                   // 非空时 /api/login 按 X-Forwarded-For 还原真实客户端 IP 限流。
    // 网络代理：经 QNetworkProxy 运行时动态配置（见 ProxyConfig）。
    // 初始代理可空（直连），运行时由 POST /proxy 动态更新，无需重启。
    ProxySpec initialProxy;       // 启动时 --proxy 指定的初始代理（默认 Direct 直连）
    std::string proxyBypass;      // --proxy-bypass <list>：逗号分隔的直连域名/CIDR 列表
                                   //（内网+外网混合目标分流；仅命令行，POST /proxy 不改它）
    bool stealthEnabled = true;   // 浏览器指纹统一（默认开启，--no-stealth 关闭）
    bool warmupEnabled = true;    // Google 会话预热（默认开启，--no-warmup 关闭）
    std::string warmupUrl = "https://www.google.com/";  // --warmup-url 自定义预热目标
    int remoteDebugPort = 0;      // Chromium --remote-debugging-port（HAR 网络录制必需；0=不开）
    std::string dataDir;          // --data-dir：持久化目录（cookies.dat / seimi.key / metrics.json）。空=默认(bin 同级 data/)
    bool uiEnabled = false;       // --gui：启用原生桌面控制台（QML + 托盘）。Windows 桌面双击
                                   //（零参数、无终端）默认进 GUI（--headless 显式退出）；无头服务不变。
    bool uiDemo = false;          // --ui-demo[=<page>]：GUI 追加「控件陈列室」页并直达指定页（验收用）。
    std::string uiDemoPage = "gallery"; // dashboard/settings/update/logs/cookies/gallery
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
        "  --gui                 launch native desktop console (QML UI + system tray).\n"
        "                        On Windows, double-clicking the exe (zero args, no\n"
        "                        parent terminal) also defaults to GUI. Settings changed\n"
        "                        in the GUI persist to data/config.json (priority: CLI\n"
        "                        flags > config.json > defaults).\n"
        "  --headless            force headless service mode (also --no-gui). Use to\n"
        "                        opt out of the double-click-GUI default from a desktop\n"
        "                        shortcut. Terminal/server/container launches are always\n"
        "                        headless regardless.\n"
        "  --no-sandbox          disable Chromium sandbox (WSL2/container/root often need this)\n"
        "  --sandbox             force Chromium sandbox on (overrides auto-detect when running as root)\n"
        "  --verbose-chromium    show Chromium/web console logs (default: known web noise filtered out)\n"
        "  --password <pw>       admin password (INSECURE: visible in `ps`/process list)\n"
        "  --password-file <f>   read password from first line of <f> (recommended over --password)\n"
        "  SEIMI_PASSWORD env    password from environment variable (recommended)\n"
        "                        Password precedence: --password > --password-file > SEIMI_PASSWORD.\n"
        "                        Empty (none given) = no password, open access.\n"
        "  --no-admin            disable built-in admin UI (default: enabled at GET /)\n"
        "  --no-ssrf             disable SSRF guard: allow rendering internal/loopback/\n"
        "                        link-local/metadata URLs. For LOCAL personal-agent use only\n"
        "                        (you trust the caller with your entire network). Default:\n"
        "                        guard ON. Never combine with a public --host.\n"
        "  --trusted-proxy <list> comma-separated trusted reverse-proxy IPs/CIDRs\n"
        "                        (e.g. 10.8.0.0/16,127.0.0.1). When set, /api/login rate-\n"
        "                        limits by the real client IP extracted from X-Forwarded-For;\n"
        "                        otherwise the TCP peer address is used (XFF ignored, forge-safe).\n"
        "  --proxy <url>         upstream proxy for all Chromium traffic. Format:\n"
        "                          http://[user:pass@]host:port\n"
        "                          socks5://[user:pass@]host:port\n"
        "                        Set via QNetworkProxy so it can be hot-swapped at\n"
        "                        runtime via POST /proxy (no restart needed).\n"
        "                        Omit for direct connection; type=direct clears it.\n"
        "  --proxy-bypass <list> comma-separated hosts that bypass --proxy and go\n"
        "                        direct: domain suffix (gwm.cn covers jira.gwm.cn),\n"
        "                        exact hostname, IP literal, or IPv4 CIDR\n"
        "                        (e.g. gwm.cn,10.0.0.0/8). Loopback is always direct.\n"
        "                        For mixed internal+external targets. Tip: if a TUN-mode\n"
        "                        proxy (Clash etc.) is running, prefer NO --proxy at all\n"
        "                        (TUN already routes; explicit proxy double-routes and\n"
        "                        loses the proxy's private-net DIRECT rules).\n"
        "  --no-stealth          disable browser fingerprint unification (default: enabled).\n"
        "                        Stealth unifies all render instances into a single Chrome\n"
        "                        desktop fingerprint (UA/screen/WebGL/canvas) to blend into\n"
        "                        the crowd and bypass basic anti-bot detection (e.g. Google).\n"
        "  --no-warmup           disable Google session warm-up at startup (default: enabled).\n"
        "                        Warm-up renders --warmup-url once to acquire Google cookies\n"
        "                        (NID/SOCS) before dispatching render tasks; re-warms every 30min.\n"
        "  --warmup-url <url>    warm-up target (default https://www.google.com/; must be http/https)\n"
        "  --remote-debugging-port <n>  Chromium remote debugging port. Enables CDP-based network\n"
        "                        recording (HAR export via /har/<task_id>, render_url record_network).\n"
        "                        When set, stealth AutomationControlled flag is SKIPPED (debug/analysis\n"
        "                        mode): anti-bot detection may be less effective. Bind to 127.0.0.1\n"
        "                        only in production (the port exposes full browser control).\n"
        "  --data-dir <path>     data directory for persistence (cookies.dat / seimi.key / metrics.json).\n"
        "                        Default: <binary_dir>/data. Set to a writable path for containerized\n"
        "                        or read-only deployments where the binary directory isn't writable.\n"
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

// 编辑距离是否 ≤1（一次插入/删除/替换）。仅用于代理 scheme 拼写纠正，输入很短。
static bool editDistanceAtMostOne(const QString& a, const QString& b) {
    if (a == b) return true;
    const int la = a.size(), lb = b.size();
    if ((la > lb ? la - lb : lb - la) > 1) return false;
    int i = 0, j = 0, diff = 0;
    while (i < la && j < lb) {
        if (a[i] == b[j]) { ++i; ++j; continue; }
        if (++diff > 1) return false;
        if (la == lb) { ++i; ++j; }   // 替换
        else if (la > lb) { ++i; }    // a 多一个字符
        else { ++j; }                 // b 多一个字符
    }
    if (i < la || j < lb) ++diff;     // 尾部剩余一个未配对字符
    return diff <= 1;
}

// 解析代理 URL 为 ProxySpec。
// 接受 "http://[user:pass@]host:port" 或 "socks5://[user:pass@]host:port"。
// scheme 缺省视为 http。未知 scheme 不再静默按 HTTP（真实排障撞过：socket5:// 拼写错误
// 静默按 HTTP 代理，恰好混合端口能用，纯 SOCKS 端口则全部流量静默失败）：
//   1) 常见拼写纠错表（socket5/socks5h/sock5 → socks5，编辑距离 2 兜不住的真实 typo）；
//   2) 编辑距离 ≤2 命中已知 scheme 也纠正（htp/socks6 类）；
//   3) https 特例：WARN 后按 HTTP（Qt 无 https 代理支持，纠正成 http 反而言过其实）；
//   4) 其余 WARN 后按 HTTP 维持容错（混合端口下可能歪打正着，不拦死服务）。
// 解析失败（无 host/port）WARN 后返回 Direct。
static ProxySpec parseProxyUrl(const std::string& s) {
    ProxySpec spec;
    QString url = QString::fromStdString(s).trimmed();
    if (url.isEmpty()) return spec;  // Direct
    int schemeEnd = url.indexOf(QStringLiteral("://"));
    QString scheme;
    QString rest;
    bool hasExplicitScheme = schemeEnd > 0;
    if (hasExplicitScheme) {
        scheme = url.left(schemeEnd).toLower();
        rest = url.mid(schemeEnd + 3);
    } else {
        scheme = QStringLiteral("http");
        rest = url;
    }
    if (scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks")) {
        spec.type = ProxySpec::Type::Socks5;
    } else if (scheme == QStringLiteral("http")) {
        spec.type = ProxySpec::Type::Http;
    } else {
        // 纠正表：真实世界撞过的拼写（socket5 编辑距离 2，距离兜底够不着）。
        static const struct { const char* typo; const char* fixed; } kTypoTable[] = {
            {"socket5", "socks5"}, {"socks5h", "socks5"}, {"sock5", "socks5"},
            {"socks4", "socks5"},
        };
        QString corrected;
        for (const auto& t : kTypoTable) {
            if (scheme == QLatin1String(t.typo)) { corrected = QLatin1String(t.fixed); break; }
        }
        if (corrected.isEmpty()
            && editDistanceAtMostOne(scheme, QStringLiteral("socks5"))) {
            corrected = QStringLiteral("socks5");
        }
        if (corrected.isEmpty()
            && editDistanceAtMostOne(scheme, QStringLiteral("http"))) {
            corrected = QStringLiteral("http");
        }
        if (!corrected.isEmpty()) {
            std::fprintf(stderr,
                         "[WARN] --proxy scheme '%s' unrecognized, auto-corrected to '%s' "
                         "(accepted: http:// or socks5:// host:port)\n",
                         scheme.toUtf8().constData(), corrected.toUtf8().constData());
            spec.type = corrected == QLatin1String("socks5") ? ProxySpec::Type::Socks5
                                                             : ProxySpec::Type::Http;
        } else if (scheme == QStringLiteral("https")) {
            std::fprintf(stderr,
                         "[WARN] --proxy scheme 'https' is not supported as a proxy scheme by "
                         "Qt (accepted: http:// or socks5://); treating as http proxy\n");
            spec.type = ProxySpec::Type::Http;
        } else {
            std::fprintf(stderr,
                         "[WARN] --proxy scheme '%s' unrecognized, falling back to http proxy "
                         "(accepted: http:// or socks5:// host:port)\n",
                         scheme.toUtf8().constData());
            spec.type = ProxySpec::Type::Http;
        }
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
        // 无有效 host/port，退化为直连（启动期容错，不致命，但必须可见——
        // 静默退化会让「以为配了代理实际直连」的排查非常痛苦）。
        std::fprintf(stderr,
                     "[WARN] --proxy '%s' has no valid host:port, falling back to DIRECT\n",
                     url.toUtf8().constData());
        spec.type = ProxySpec::Type::Direct;
        spec.host.clear();
        spec.port = 0;
        spec.user.clear();
        spec.pass.clear();
    }
    return spec;
}

static Config parseArgs(int argc, char** argv, AppConfig::CliOverrides& ov) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int& out) {
            if (i + 1 < argc) { out = std::atoi(argv[++i]); }
        };
        if (a == "--help" || a == "-h") { printUsage(); std::exit(0); }
        else if (a == "--http-port" && i + 1 < argc) { c.httpPort = std::atoi(argv[++i]); ov.httpPort = true; }
        else if (a == "--ws-port" && i + 1 < argc)   { c.wsPort = std::atoi(argv[++i]); ov.wsPort = true; }
        else if (a == "--mcp-port" && i + 1 < argc)  { c.mcpPort = std::atoi(argv[++i]); ov.mcpPort = true; }
        else if (a == "--host" && i + 1 < argc) { c.host = argv[++i]; c.hostExplicit = true; ov.host = true; }
        else if (a == "--concurrency") { next(c.concurrency); ov.concurrency = true; }
        else if (a == "--http-threads") { next(c.httpThreads); ov.httpThreads = true; }
        else if (a == "--settle-ms") { next(c.settleDefaultMs); ov.settleMs = true; }
        else if (a == "--load-timeout-ms" && i + 1 < argc) { c.loadTimeoutMs = std::atoll(argv[++i]); ov.loadTimeoutMs = true; }
        else if (a == "--windowed") c.windowed = true;
        else if (a == "--gui") c.uiEnabled = true;
        else if (a == "--headless" || a == "--no-gui") c.uiEnabled = false;   // 双击默认 GUI 的显式退出开关
        else if (a.rfind("--ui-demo", 0) == 0) {
            c.uiEnabled = true; c.uiDemo = true;
            if (a.size() > 10) c.uiDemoPage = a.substr(10);   // --ui-demo=settings
        }
        else if (a == "--no-sandbox") { c.noSandbox = true; c.sandboxExplicit = true; }
        else if (a == "--sandbox")    { c.noSandbox = false; c.sandboxExplicit = true; }
        else if (a == "--verbose-chromium") c.verboseChromium = true;
        else if (a == "--password" && i + 1 < argc) { c.adminPassword = argv[++i]; ov.password = true; }
        else if (a == "--password-file" && i + 1 < argc) { c.passwordFile = argv[++i]; ov.password = true; }
        else if (a == "--no-admin")   { c.adminEnabled = false; ov.adminEnabled = true; }
        else if (a == "--no-ssrf")    { c.ssrfGuardEnabled = false; ov.ssrfGuard = true; }
        else if (a == "--trusted-proxy" && i + 1 < argc) { c.trustedProxies = argv[++i]; ov.trustedProxies = true; }
        else if (a == "--proxy" && i + 1 < argc) { c.initialProxy = parseProxyUrl(argv[++i]); ov.proxy = true; }
        else if (a == "--proxy-bypass" && i + 1 < argc) { c.proxyBypass = argv[++i]; ov.proxyBypass = true; }
        else if (a == "--no-stealth") { c.stealthEnabled = false; ov.stealth = true; }
        else if (a == "--no-warmup")  { c.warmupEnabled = false; ov.warmup = true; }
        else if (a == "--warmup-url" && i + 1 < argc) { c.warmupUrl = argv[++i]; ov.warmupUrl = true; }
        else if (a == "--remote-debugging-port" && i + 1 < argc) { c.remoteDebugPort = std::atoi(argv[++i]); ov.remoteDebugPort = true; }
        else if (a == "--data-dir" && i + 1 < argc) { c.dataDir = argv[++i]; ov.dataDir = true; }
        else if (a == "--apply-update" || a == "--updated-from") {
            // 由 main() 最前置分支处理；parseArgs 静默吞掉其余参数。
        }
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

// 「双击默认 GUI」判定结果：Windows 桌面双击/Run 对话框启动（零参数、无终端父进程、
// 无 std 重定向）时为 true，由 main() 在 parseArgs 后叠加到 cfg.uiEnabled。
// 终端 ./seimi-render.exe、重定向/服务包装器启动、非 Windows 平台恒为 false。
static bool g_desktopAutoGui = false;

#ifdef _WIN32
// GUI 子系统的控制台归属（双模式：桌面 GUI 与无头服务共用一个 exe）：
//   1) std 句柄已有效（终端里跑且带重定向 `> log`）→ Redirected：不碰控制台，输出走文件/管道；
//   2) 无有效句柄 + 父进程有控制台（终端里直接跑）→ ParentConsole：AttachConsole 挂回父终端，
//      banner/日志照常显示；
//   3) 无有效句柄 + 无父终端（双击/Run 对话框）→ Detached：guiIfDetached 时不建控制台
//      （桌面 GUI 是唯一可见存在）；无头模式 AllocConsole 自建（与旧控制台子系统一致）。
// 返回启动来源，调用方据此落实「双击默认 GUI」；之后统一切 UTF-8 代码页。
enum class WinLaunchCtx { Redirected, ParentConsole, Detached };
static WinLaunchCtx setupWindowsConsole(bool guiIfDetached) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool haveRedirect = out != NULL && out != INVALID_HANDLE_VALUE;
    if (haveRedirect) return WinLaunchCtx::Redirected;

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        return WinLaunchCtx::ParentConsole;
    }
    if (!guiIfDetached) {
        AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    return WinLaunchCtx::Detached;
}
#endif

int main(int argc, char** argv) {
    using namespace seimi;

#ifdef _WIN32
    // --apply-update 子进程保持零输出零控制台（在控制台策略之前判断）。
    bool applyUpdateMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--apply-update") == 0) applyUpdateMode = true;
    }
    if (!applyUpdateMode) {
        bool guiFlag = false, headlessFlag = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--gui") == 0) guiFlag = true;
            else if (std::strcmp(argv[i], "--headless") == 0
                     || std::strcmp(argv[i], "--no-gui") == 0) headlessFlag = true;
        }
        // 「双击默认 GUI」：零参数 + 无终端父进程 + 无 std 重定向 = 桌面双击/Run 对话框。
        // 预判传给 setup 决定是否建控制台，再用返回的启动来源确认——终端里 ./seimi-render.exe
        //（ParentConsole）与重定向启动（Redirected）都会收回，服务/容器部署行为不变。
        const bool autoGuiCandidate = !guiFlag && !headlessFlag && argc == 1;
        const WinLaunchCtx launchCtx = setupWindowsConsole(autoGuiCandidate);
        g_desktopAutoGui = autoGuiCandidate && launchCtx == WinLaunchCtx::Detached;
    }

    // Windows 控制台默认代码页 936(GBK)，把 qWarning/mcpLog/fprintf 输出的 UTF-8 字节
    // 当 GBK 解释 → 含中文的日志（如未 URL-encode 的 url）乱码。
    // 切到 UTF-8 代码页。只影响当前进程的控制台，进程退出后不改变系统默认。
    // 必须在任何输出（banner/日志）之前完成。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // --apply-update 子命令：更新交接的子进程模式（纯文件操作 + 启动新版本），
    // 必须在 QApplication/Chromium 之前的最早分支处理。父进程退出后由它做目录交换。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--apply-update") == 0) {
            QStringList applyArgs;
            for (int j = 1; j < argc; ++j) applyArgs << QString::fromLocal8Bit(argv[j]);
            return runApplyUpdate(applyArgs);
        }
    }

    AppConfig::CliOverrides cliOv;
    Config cfg = parseArgs(argc, argv, cliOv);
    // Windows 桌面双击（零参数、Detached）默认进 GUI；--headless/--no-gui 已在预扫描排除。
    if (g_desktopAutoGui && !cfg.uiEnabled) cfg.uiEnabled = true;
    g_verboseChromium = cfg.verboseChromium;
    qInstallMessageHandler(noiseFilter);  // 安装降噪过滤器（在 QApplication 之前）
    // warmup URL 校验：仅 http/https（运维配置项，不过 SSRF——同 --proxy 语义）。
    if (cfg.warmupEnabled
        && cfg.warmupUrl.rfind("http://", 0) != 0
        && cfg.warmupUrl.rfind("https://", 0) != 0) {
        std::fprintf(stderr, "FATAL: --warmup-url must be http/https: %s\n", cfg.warmupUrl.c_str());
        return 1;
    }

    // ---------- 配置持久化（data/config.json，优先级 默认 < 文件 < CLI）----------
    // 注意时机：合并必须发生在 Chromium flag 注入之前——proxy-bypass/stealth/CDP 端口
    // 都参与启动期 flag 快照。dataDir 此时 QApplication 未建，按 argv[0] 推 bin 目录
    //（QApplication 建立后 applicationDirPath 与之一致）。
    {
        QString preDataDir = !cfg.dataDir.empty()
            ? QString::fromStdString(cfg.dataDir)
            : QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath() + QStringLiteral("/data");
        // 上次更新残留清理（<binDir>.old / updater 副本）；失败仅告警。
        cleanupUpdateResidue(QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath(), preDataDir);
        AppConfig appCfg = AppConfig::load(preDataDir);
        // —— 文件值叠加（仅 CLI 未显式覆盖的字段）——
        if (appCfg.host && !cliOv.host) cfg.host = appCfg.host->toStdString();
        if (appCfg.httpPort && !cliOv.httpPort) cfg.httpPort = *appCfg.httpPort;
        if (appCfg.wsPort && !cliOv.wsPort) cfg.wsPort = *appCfg.wsPort;
        if (appCfg.mcpPort && !cliOv.mcpPort) cfg.mcpPort = *appCfg.mcpPort;
        if (appCfg.concurrency && !cliOv.concurrency) cfg.concurrency = *appCfg.concurrency;
        if (appCfg.httpThreads && !cliOv.httpThreads) cfg.httpThreads = *appCfg.httpThreads;
        if (appCfg.settleMs && !cliOv.settleMs) cfg.settleDefaultMs = *appCfg.settleMs;
        if (appCfg.loadTimeoutMs && !cliOv.loadTimeoutMs) cfg.loadTimeoutMs = *appCfg.loadTimeoutMs;
        if (appCfg.adminEnabled && !cliOv.adminEnabled) cfg.adminEnabled = *appCfg.adminEnabled;
        if (appCfg.ssrfGuard && !cliOv.ssrfGuard) cfg.ssrfGuardEnabled = *appCfg.ssrfGuard;
        if (appCfg.stealth && !cliOv.stealth) cfg.stealthEnabled = *appCfg.stealth;
        if (appCfg.warmup && !cliOv.warmup) cfg.warmupEnabled = *appCfg.warmup;
        if (appCfg.warmupUrl && !cliOv.warmupUrl) cfg.warmupUrl = appCfg.warmupUrl->toStdString();
        if (appCfg.remoteDebugPort && !cliOv.remoteDebugPort) cfg.remoteDebugPort = *appCfg.remoteDebugPort;
        if (appCfg.trustedProxies && !cliOv.trustedProxies) cfg.trustedProxies = appCfg.trustedProxies->toStdString();
        if (appCfg.proxyBypass && !cliOv.proxyBypass) cfg.proxyBypass = appCfg.proxyBypass->toStdString();
        if (appCfg.proxy && !cliOv.proxy
            && cfg.initialProxy.type == ProxySpec::Type::Direct) {
            cfg.initialProxy = *appCfg.proxy;
        }
        // 密码：--password/--password-file/SEIMI_PASSWORD 之后，config.json 兜底。
        if (cfg.adminPassword.empty() && appCfg.password) {
            cfg.adminPassword = appCfg.password->toStdString();
        }
        // —— 数据目录重定向（一跳）：config.json 指定的 data_dir 只重定向「数据落盘」
        //    （cookies/metrics/anomalies/updates），config.json 本身留在引导目录——
        //    保证重定向永远一跳可解、无环。CLI --data-dir 显式给定时整体让位。
        QString effectiveDataDir = preDataDir;
        if (!cliOv.dataDir && appCfg.dataDirOverride && !appCfg.dataDirOverride->trimmed().isEmpty()) {
            const QString redirected = appCfg.dataDirOverride->trimmed();
            if (QDir(redirected).absolutePath() != QDir(preDataDir).absolutePath()) {
                effectiveDataDir = redirected;
                cfg.dataDir = effectiveDataDir.toStdString();
                std::fprintf(stdout, "[config] data dir redirected by config.json: %s -> %s\n",
                             qPrintable(QDir(preDataDir).absolutePath()),
                             qPrintable(QDir(effectiveDataDir).absolutePath()));
            }
        }
        // 二次钳制（文件值与 CLI 同一护栏）。
        cfg.concurrency = std::max(1, cfg.concurrency);
        cfg.httpThreads = std::max(2, cfg.httpThreads);
        cfg.settleDefaultMs = std::max(0, cfg.settleDefaultMs);
        cfg.loadTimeoutMs = std::max(1000LL, cfg.loadTimeoutMs);
        cfg.httpPort = std::clamp(cfg.httpPort, 1, 65535);
        cfg.wsPort = std::clamp(cfg.wsPort, 1, 65535);
        cfg.mcpPort = std::clamp(cfg.mcpPort, 1, 65535);
        cfg.remoteDebugPort = std::clamp(cfg.remoteDebugPort, 0, 65535);
        // GUI 模式需要展示文件态配置（SettingsPage 编辑对象），存到 main 作用域。
        // （appCfg 的 m_dataDir/m_filePath 保持引导目录：后续 save() 永远写回引导
        //   config.json，data_dir 指针不丢失；数据文件走 effectiveDataDir。）
        g_appConfig = appCfg;
        g_effectiveDataDir = effectiveDataDir;
        // 更新残留清理跟随实际生效目录（updater 副本/ .old 住在 effective/updates）。
        if (effectiveDataDir != preDataDir) {
            cleanupUpdateResidue(QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath(),
                                 effectiveDataDir);
        }
    }

    // 平台选择：无头渲染服务默认 offscreen（无窗口、不依赖窗口系统、事件循环稳定）。
    // --gui 启用原生桌面控制台（QML 窗口 + 托盘，需要真实平台）；--windowed 调试用。
    if (!cfg.windowed && !cfg.uiEnabled) {
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

    // CDP 网络录制：开启 Chromium remote debugging port（HAR 录制必需）。
    // Qt WebEngine 底层 = Chromium，--remote-debugging-port 开出 CDP 端点供本进程内嵌的
    // CdpClient 连接订阅 Network domain 事件。默认绑 127.0.0.1（Chromium 默认行为）。
    if (cfg.remoteDebugPort > 0) {
        QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        if (existing.indexOf("--remote-debugging-port") < 0) {
            QByteArray flags = existing.trimmed();
            if (!flags.isEmpty()) flags += ' ';
            flags += "--remote-debugging-port=" + QByteArray::number(cfg.remoteDebugPort);
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
            std::fprintf(stderr,
                "NOTE: Chromium remote debugging port %d enabled (debug/analysis mode).\n"
                "      Stealth AutomationControlled flag will be SKIPPED to avoid CDP conflict —\n"
                "      anti-bot detection may be less effective. Ensure the port is not exposed\n"
                "      to untrusted networks (it grants full browser control).\n",
                cfg.remoteDebugPort);
        }
    }

    // 网络代理：经 QNetworkProxy 运行时动态配置（见 ProxyConfig）。
    // Qt WebEngine 轮询全局应用代理，运行时修改后 Chromium 在数秒内切换。具体 apply 由 RenderPool GUI 线程定时器驱动。
    // --proxy-bypass 分流的渲染侧实现：实测（2026-09-15）QtWebEngine 渲染网络栈**不**按请求
    // 查询 QNetworkProxyFactory（只轮询全局单值 applicationProxy），QNetworkProxyFactory 只对
    // QNAM（POST /fetch）生效。要让 Chromium 渲染按 host 分流，唯一路径是 Chromium 原生
    // --proxy-server + --proxy-bypass-list（经 QTWEBENGINE_CHROMIUM_FLAGS 注入，实测对当前
    // Qt 构建的网络服务生效）。代价：flag 是启动期快照，bypass 模式下渲染代理不随 POST /proxy
    // 热切换（/fetch 侧仍可热切换）；需要热切换就别配 --proxy-bypass（改 direct + 系统级路由）。
    // Qt 侧 ProxyConfig 仍持有同值代理 + bypass（SeimiProxyFactory 给 /fetch 的 QNAM 分流，
    // 与渲染行为一致）。Chromium flag 代理不支持内联凭据——带认证的代理 + bypass 组合不受支持。
    if (cfg.initialProxy.type != ProxySpec::Type::Direct && !cfg.proxyBypass.empty()) {
        const char* scheme = cfg.initialProxy.type == ProxySpec::Type::Socks5 ? "socks5" : "http";
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS").trimmed();
        if (!flags.isEmpty()) flags += ' ';
        flags += QByteArrayLiteral("--proxy-server=") + scheme + "://"
                 + QByteArray::fromStdString(cfg.initialProxy.host) + ":"
                 + QByteArray::number(cfg.initialProxy.port);
        // Chromium bypass 语法与我们的语义兼容：域名后缀条目（gwm.cn 匹配自身与 *.gwm.cn）、
        // IP 字面量、IPv4 CIDR；回环由 Chromium 隐式规则天然包含。
        flags += QByteArrayLiteral(" --proxy-bypass-list=")
                 + QByteArray::fromStdString(cfg.proxyBypass);
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
        std::fprintf(stderr,
            "NOTE: --proxy-bypass active: Chromium render traffic splits per host "
            "(bypassed hosts go direct).\n"
            "      The render-side proxy is FIXED at startup (Chromium flag snapshot); "
            "POST /proxy hot-swap only affects /fetch in this mode.\n");
    }


    // offscreen 模式下的软件渲染（Linux + macOS，解决无头截图白屏）：
    // Chromium 默认硬件 GPU 合成，结果落在独立 GPU 进程的 surface，不回写 QWidget 的 CPU
    // backing store，而 view->grab() 读的正是 backing store → 截图空白。
    // 注入 --disable-gpu 强制软件合成，帧光栅化进 backing store，grab() 才能取到真实像素。
    // 平台差异：Linux/WSL/容器无 GPU 或驱动不全，必须软件渲染；macOS Metal 合成同样不落
    // backing store，需软件渲染；Windows DirectX offscreen 合成会回写 backing store，截图正常，不注入。
    // GUI 模式（--gui）不注入：QML 场景图需要 GPU。
    // 用户可通过 QTWEBENGINE_CHROMIUM_FLAGS 覆盖（已含相关 flag 则不重复注入）。
    if (!cfg.windowed && !cfg.uiEnabled) {
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
    //
    // 关键互斥：开启 --remote-debugging-port（CDP 网络录制）时跳过 AutomationControlled——
    // 该 flag 会抹除 CDP 痕迹，与录制所需的 CDP 端点直接冲突。开端口 = 明确进入调试/分析模式，
    // 接受反爬能力下降。其余 stealth 层（UA 统一、stealth.js、Client Hints）仍生效，减少指纹漂移。
    if (cfg.stealthEnabled) {
        QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        QByteArray flags = existing.trimmed();

        // 核心：禁用 AutomationControlled（抹除 webdriver + CDP 痕迹）。
        // 仅在未开 remote debugging port 时注入（CDP 录制与该 flag 互斥）。
        const bool cdpActive = (cfg.remoteDebugPort > 0);
        if (!cdpActive && existing.indexOf("AutomationControlled") < 0) {
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

    // 优雅退出：接管 SIGINT/SIGTERM / Windows 控制台事件（Ctrl+C、关控制台窗口、
    // 服务停止），转入事件循环走 aboutToQuit 关机链。
    g_appForQuit = &app;
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, posixSignalHandler);
    std::signal(SIGTERM, posixSignalHandler);
#endif

    // 持久化目录：--data-dir 显式给定；缺省 = 二进制同级 data/。
    // 存放 cookies.dat（加密 cookie）/ seimi.key（机器绑定盐）/ metrics.json（累计指标）。
    // 容器/只读环境（二进制目录不可写）需显式指定到可写卷。
    QString dataDir = cfg.dataDir.empty()
        ? (QCoreApplication::applicationDirPath() + QStringLiteral("/data"))
        : QString::fromStdString(cfg.dataDir);

    // 渲染队列：全局线程安全中枢。
    // 传入 data 目录启用指标持久化（重启自动恢复累计统计，跨重启累加）。
    RenderQueue queue(dataDir);

    // Cookie 同步中枢：浏览器插件通过 POST /cookies 灌入登录态，
    // 由 RenderPool 在 GUI 线程周期 apply 到共享 profile。
    // 传入 data 目录启用加密持久化（重启自动恢复登录态）。
    CookieStore cookies(dataDir);

    // 网络代理配置中枢（GUI 线程 apply）：HTTP 线程经 POST /proxy 写入，
    // 由 RenderPool 的 GUI 线程定时器周期调 apply() 把新代理写入
    // QNetworkProxy::setApplicationProxy（Qt WebEngine 轮询它，运行时动态生效）。
    ProxyConfig proxyConfig;
    if (cfg.initialProxy.type != ProxySpec::Type::Direct) {
        proxyConfig.setProxy(cfg.initialProxy);
    }
    // --proxy-bypass：必须在首次 apply() 前配置（决定 apply 走 factory 还是全局单值）。
    if (!cfg.proxyBypass.empty()) {
        proxyConfig.setBypass(QString::fromStdString(cfg.proxyBypass));
    }

    // HAR 网络录制中枢（仅当 --remote-debugging-port 开启时启用）。
    // HarStore 是线程安全的归档存储（HTTP 线程查询 + GUI 线程归档），
    // HarRecorder 是 GUI 线程的录制中枢（管理 CdpClient + 聚合 CDP 事件为 HAR）。
    HarStore harStore;
    HarRecorder* harRecorder = nullptr;
    if (cfg.remoteDebugPort > 0) {
        harRecorder = new HarRecorder(cfg.remoteDebugPort, &harStore, &app);
    }

    // 渲染池（GUI 线程）：N 个 WebEngine 并发。
    RenderPool pool(&queue, cfg.concurrency, cfg.loadTimeoutMs, &app);
    pool.setCookieStore(&cookies);
    pool.setProxyConfig(&proxyConfig);
    pool.setStealthEnabled(cfg.stealthEnabled);
    pool.setSsrfGuardEnabled(cfg.ssrfGuardEnabled);
    pool.setWarmupEnabled(cfg.warmupEnabled);
    pool.setWarmupUrl(QString::fromStdString(cfg.warmupUrl));
    if (harRecorder) pool.setHarRecorder(harRecorder);

    // WebSocket 服务（GUI 线程）：支持 render 请求与 subscribe 订阅。
    // 启用密码时传入同一确定性 token，WS 端口同样受保护（见 WsServer 鉴权）。
    WsServer ws(&queue, cfg.settleDefaultMs, HttpServer::computeToken(cfg.adminPassword), &app);
    ws.setSsrfGuardEnabled(cfg.ssrfGuardEnabled);
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
            // 登录态可见性：final_url / auth_wall 随推送透出，agent 据此判断掉登录。
            QString finalUrl = t ? t->finalUrl : QString();
            bool authWall = t && t->authWall;
            ws.notifyFinished(id, state, blocked, finalUrl, authWall);
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
    // 注入启动命令行参数（含脱敏），供管理后台运行环境展示完整启动配置。
    env.setLaunchArgs(QCoreApplication::arguments());

    // HTTP server 跑在独立线程（httplib::listen 阻塞）。
    HttpServer http(&queue, &cookies, cfg.settleDefaultMs,
                    adminUi, cfg.adminPassword, &app);
    http.setEnvironment(&env);
    http.setSsrfGuardEnabled(cfg.ssrfGuardEnabled);
    // 可信反代网段：非空时 /api/login 按 X-Forwarded-For 还原真实 IP 限流（反代部署用）。
    if (!cfg.trustedProxies.empty()) {
        http.setTrustedProxies(cfg.trustedProxies);
    }
    // 网络代理配置：注册 GET/POST/DELETE /proxy 接口。
    http.setProxyConfig(&proxyConfig);
    // /fetch 的 response_format=file 落盘目录（data-dir/fetch/）。
    http.setFetchSaveDir(dataDir + QStringLiteral("/fetch"));
    // HAR 录制产物查询：GET /har/<task_id>。仅在 --remote-debugging-port 开启时挂载
    //（HarStore 虽然总是实例化，但未开 CDP 时没有录制能力，/har 应返回 503 而非 404）。
    if (harRecorder) {
        http.setHarStore(&harStore);
    }

    // MCP server（供 Claude Code 等 agent 工具接入）。工具内部经 HTTP 调本机渲染 API。
    // 启用密码时工具调渲染 API 带同一确定性 token 鉴权。
    // MCP 端口绑定：用户未显式 --host 时强制回环，避免误开公网。需远程 MCP 显式 --host 0.0.0.0。
    // 在 httpThread 启动之前构造 mcp，以便 http 的 /status 能通过 setMcpServer 稳定引用到它
    //（setMcpServer 必须在 http.start() 即路由注册前完成，否则存在 /status 抢先读不到 m_mcp 的竞态）。
    std::string mcpHost = cfg.hostExplicit ? cfg.host : std::string("127.0.0.1");
    McpServer mcp;
    mcp.start(mcpHost, cfg.mcpPort, "127.0.0.1", cfg.httpPort,
              HttpServer::computeToken(cfg.adminPassword));
    http.setMcpServer(&mcp);

    std::thread httpThread([&]() {
        if (!http.start(cfg.host, cfg.httpPort, cfg.httpThreads)) {
            std::fprintf(stderr, "FATAL: HTTP server failed to listen on %s:%d\n",
                         cfg.host.c_str(), cfg.httpPort);
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        }
    });

    // 指标持久化：定时把累计统计落盘（跨重启累加）。GUI 线程 QTimer。
    // 崩溃最多丢 30s 数据；退出时 aboutToQuit 再 flush 一次。
    QTimer metricsPersistTimer;
    metricsPersistTimer.setInterval(30 * 1000);
    QObject::connect(&metricsPersistTimer, &QTimer::timeout, [&queue]() {
        queue.metrics().saveToDisk();
    });
    metricsPersistTimer.start();

    // 优雅退出：信号到来时按序关闭。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        queue.stop();   // 唤醒渲染消费者 + 长轮询线程
        pool.stop();
        http.stop();    // 让 httplib::listen 返回
        if (httpThread.joinable()) httpThread.join();
        mcp.stop();
        cookies.flush();  // 退出前确保 cookie 持久化落盘
        queue.metrics().saveToDisk();  // 退出前确保指标持久化落盘
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
    // SSRF 防护状态提示（默认启用，--no-ssrf 关闭时显眼标注）。
    if (!cfg.ssrfGuardEnabled) {
        std::fprintf(stdout, "  SSRF guard : DISABLED (--no-ssrf)\n");
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
    // --no-ssrf：关闭 SSRF 防护是高危配置，给显著警告（尤其叠加对外暴露时）。
    if (!cfg.ssrfGuardEnabled) {
        std::fprintf(stderr,
            "WARNING: SSRF guard disabled (--no-ssrf) — renders can reach internal,\n"
            "         loopback, and cloud-metadata endpoints. Intended ONLY for trusted\n"
            "         local agents; combine with a password even on localhost.\n");
        if (cfg.host != "127.0.0.1" && cfg.host != "localhost") {
            std::fprintf(stderr,
                "DANGER : --no-ssrf combined with --host %s (non-loopback) exposes your\n"
                "         entire internal network to anyone who can reach this port.\n"
                "         Add --password / --password-file / SEIMI_PASSWORD immediately.\n",
                cfg.host.c_str());
        }
    }
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

    // ---------- 原生 GUI（--gui）：QML 控制台 + 托盘 ----------
    // 装配点在全部服务组件就绪之后、pool.start() 之前；GUI 独占持有 config.json 文件态，
    // 页面数据经线程安全快照 API 读取，不触碰 WebEngine。
    std::unique_ptr<GuiApp> gui;
    if (cfg.uiEnabled) {
        QVariantMap effective;
        effective.insert(QStringLiteral("host"), QString::fromStdString(cfg.host));
        effective.insert(QStringLiteral("httpPort"), cfg.httpPort);
        effective.insert(QStringLiteral("wsPort"), cfg.wsPort);
        effective.insert(QStringLiteral("mcpPort"), cfg.mcpPort);
        effective.insert(QStringLiteral("concurrency"), cfg.concurrency);
        effective.insert(QStringLiteral("httpThreads"), cfg.httpThreads);
        effective.insert(QStringLiteral("settleMs"), cfg.settleDefaultMs);
        effective.insert(QStringLiteral("loadTimeoutMs"), double(cfg.loadTimeoutMs));
        effective.insert(QStringLiteral("password"), QString::fromStdString(cfg.adminPassword));
        effective.insert(QStringLiteral("adminEnabled"), cfg.adminEnabled);
        effective.insert(QStringLiteral("ssrfGuard"), cfg.ssrfGuardEnabled);
        effective.insert(QStringLiteral("stealth"), cfg.stealthEnabled);
        effective.insert(QStringLiteral("warmup"), cfg.warmupEnabled);
        effective.insert(QStringLiteral("warmupUrl"), QString::fromStdString(cfg.warmupUrl));
        effective.insert(QStringLiteral("remoteDebugPort"), cfg.remoteDebugPort);
        effective.insert(QStringLiteral("trustedProxies"), QString::fromStdString(cfg.trustedProxies));
        const ProxySpec pxy = proxyConfig.snapshot();
        const char* pxyType = pxy.type == ProxySpec::Type::Socks5 ? "socks5"
                            : pxy.type == ProxySpec::Type::Http   ? "http" : "direct";
        effective.insert(QStringLiteral("proxyType"), QLatin1String(pxyType));
        effective.insert(QStringLiteral("proxyHost"), QString::fromStdString(pxy.host));
        effective.insert(QStringLiteral("proxyPort"), int(pxy.port));
        effective.insert(QStringLiteral("proxyUser"), QString::fromStdString(pxy.user));
        effective.insert(QStringLiteral("proxyPass"), QString::fromStdString(pxy.pass));
        effective.insert(QStringLiteral("proxyBypass"), QString::fromStdString(cfg.proxyBypass));
        effective.insert(QStringLiteral("dataDir"), g_effectiveDataDir);

        QVariantMap cliFlags;
        auto putFlag = [&](const char* k, bool v) { cliFlags.insert(QLatin1String(k), v); };
        putFlag("host", cliOv.host);           putFlag("httpPort", cliOv.httpPort);
        putFlag("wsPort", cliOv.wsPort);       putFlag("mcpPort", cliOv.mcpPort);
        putFlag("concurrency", cliOv.concurrency);
        putFlag("httpThreads", cliOv.httpThreads);
        putFlag("settleMs", cliOv.settleMs);   putFlag("loadTimeoutMs", cliOv.loadTimeoutMs);
        putFlag("password", cliOv.password);   putFlag("adminEnabled", cliOv.adminEnabled);
        putFlag("ssrfGuard", cliOv.ssrfGuard); putFlag("trustedProxies", cliOv.trustedProxies);
        putFlag("proxy", cliOv.proxy);         putFlag("proxyBypass", cliOv.proxyBypass);
        putFlag("stealth", cliOv.stealth);     putFlag("warmup", cliOv.warmup);
        putFlag("warmupUrl", cliOv.warmupUrl); putFlag("remoteDebugPort", cliOv.remoteDebugPort);
        putFlag("dataDir", cliOv.dataDir);

        GuiApp::Params gp;
        gp.cfg = g_appConfig;
        gp.effectiveSettings = effective;
        gp.cliFlags = cliFlags;
        gp.dataDir = dataDir;
        gp.queue = &queue;
        gp.proxy = &proxyConfig;
        gp.cookies = &cookies;
        gp.mcp = &mcp;
        gp.env = &env;
        gp.host = QString::fromStdString(cfg.host);
        gp.httpPort = cfg.httpPort;
        gp.wsPort = cfg.wsPort;
        gp.mcpPort = cfg.mcpPort;
        gp.password = QString::fromStdString(cfg.adminPassword);
        gp.windowed = cfg.windowed;
        gp.uiDemo = cfg.uiDemo;
        gp.uiDemoPage = QString::fromStdString(cfg.uiDemoPage);
        gui = std::make_unique<GuiApp>(gp);
        gui->start();
#ifdef _WIN32
        // 桌面模式释放控制台：Explorer 双击启动不再弹黑窗，日志改走 GUI 日志页。
        // （终端里启动时本进程脱离终端，终端本身不受影响；headless 模式不释放。）
        ::FreeConsole();
#endif
    }

    // 启动渲染池（必须在事件循环开始前建好 page，由 start() 完成）。
    pool.start();

    // 关键：禁止"最后一个窗口关闭即退出"。截图需 view->show() 激活渲染管线，destroyPage()
    // 时 view 被 deleteLater 关闭——若不禁用此默认行为，首个任务完成后 view 关闭会触发
    // lastWindowClosed → app.quit()。无头服务只响应显式信号。
    app.setQuitOnLastWindowClosed(false);

    int rc = app.exec();
    return rc;
}
