// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "McpServer.h"

// 版本号统一来自 CMakeLists.txt 的 project(VERSION)，编译期由 SEIMI_VERSION 宏注入。
// 这里补 fallback：不经过 CMake 直接编译本文件时（如单文件 lint/IDE 分析）也能解析。
#ifndef SEIMI_VERSION
#define SEIMI_VERSION "unknown"
#endif

#include "TokenCompare.h"

#include <httplib.h>
#include <mcp_server.h>
#include <mcp_tool.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <thread>

namespace seimi {

// MCP 控制台日志。统一带 [MCP] 前缀 + 本地时间戳，与 main.cpp / RenderPool 的 fprintf 风格一致，
// 便于从控制台日志中一眼区分 MCP 调用与 HTTP/WS 入口。
// 用 stderr（服务型进程常规日志流），每行强制 flush，确保崩溃前日志可见。
static void mcpLog(const char* fmt, ...) {
    char timebuf[24];
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(stderr, "[MCP] %s ", timebuf);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    // 每行补换行，调用方无需手动加
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// URL 百分号编码（RFC 3986 unreserved 保留，其余编码）。搜索引擎 query 需要。
static std::string urlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(char(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Base64 编码（MCP image content 需要）。手写避免拉额外依赖。
static std::string base64Encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// 内部实现：持有 cpp-mcp server 实例与渲染 API 的连接信息。
struct McpServer::Impl {
    std::string mcpHost;
    int mcpPort = 0;
    std::string renderHost;
    int renderPort = 0;
    std::string authToken;       // 非空时调渲染 API 带 Authorization: Bearer
    std::unique_ptr<mcp::server> srv;
    std::thread thr;
    std::atomic<bool> running{false};

    // 通过 HTTP 调本进程渲染 API，返回响应体；失败返回空串。
    // 启用了密码时带上 Authorization: Bearer <authToken>。
    std::string httpGet(const std::string& path) {
        httplib::Client cli(renderHost, renderPort);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(70);
        httplib::Headers h;
        if (!authToken.empty()) h.emplace("Authorization", "Bearer " + authToken);
        auto r = cli.Get(path.c_str(), h);
        return r ? r->body : std::string();
    }
    std::string httpPost(const std::string& path, const std::string& body) {
        httplib::Client cli(renderHost, renderPort);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(70);
        httplib::Headers h{{"Content-Type", "application/json"}};
        if (!authToken.empty()) h.emplace("Authorization", "Bearer " + authToken);
        auto r = cli.Post(path.c_str(), h, body, "application/json");
        return r ? r->body : std::string();
    }
};

McpServer::McpServer()
    : m_impl(std::make_unique<Impl>()) {}

McpServer::~McpServer() {
    stop();
}

// 把渲染 API 返回的状态 JSON 包成 MCP tool result（text 内容数组）。
// MCP 规范要求 result 是 {"content":[{"type":"text","text":...}]}。
static mcp::json toTextContent(const std::string& text) {
    return mcp::json::array({{ {"type", "text"}, {"text", text} }});
}
// MCP image content：base64 编码的图片（data 不含 data: 前缀，mimeType 单独给）。
// MCP 规范：{"type":"image","data":"<base64>","mimeType":"image/png"}。
static mcp::json toImageContent(const std::string& base64Data, const std::string& mimeType) {
    return { {"type", "image"}, {"data", base64Data}, {"mimeType", mimeType} };
}

bool McpServer::start(const std::string& host, int port,
                      const std::string& renderHost, int renderPort,
                      const std::string& authToken) {
    m_impl->mcpHost = host;
    m_impl->mcpPort = port;
    m_impl->renderHost = renderHost;
    m_impl->renderPort = renderPort;
    m_impl->authToken = authToken;

    mcp::server::configuration conf;
    conf.host = host;
    conf.port = port;
    // 会话容量与回收。seimi-render 的工具无状态，cpp-mcp 已 patch 为「过期 session 自动重建」
    //（client 带任何 session id 来调工具都会成功，沿用其 id 就地新建）。故 session 配置
    // 只影响内存堆积，不影响可用性。max_sessions=64 留多 agent 并发余量；session_timeout=3600
    // 让 maintenance 线程回收空闲超 1 小时的 session。
    conf.max_sessions = 64;
    conf.session_timeout = 3600;

    m_impl->srv = std::make_unique<mcp::server>(conf);
    m_impl->srv->set_server_info("seimi-render", SEIMI_VERSION);

    // [seimi patch] 接入层鉴权：启用密码时 MCP 端口每个请求都要带 token。
    // cpp-mcp 上游 set_auth_handler 是空 stub，已在 fork 的 mcp_server.cpp 补上 enforce_auth_。
    // handler 签名 (token, path) -> bool，用恒定时间比较 token。
    if (!authToken.empty()) {
        std::string expected = authToken;  // 拷贝，避免捕获悬空引用
        m_impl->srv->set_auth_handler(
            [expected](const std::string& token, const std::string&) -> bool {
                if (token.empty()) return false;
                return constantTimeEquals(token, expected);
            });
    }

    // 声明服务器能力。capabilities_ 默认 null，新版 MCP client（Zod schema 校验）要求
    // initialize 响应的 capabilities 必须是 object，遇 null 直接拒绝重连。显式设成 object。
    m_impl->srv->set_capabilities(mcp::json::object({
        {"tools", mcp::json::object({
            {"listChanged", false}
        })}
    }));

    // impl 在 MCP server 生命周期内稳定不变（m_impl 直到析构才销毁），
    // 工具 handler 在 server 线程里通过它调渲染 API，安全。
    Impl* impl = m_impl.get();

    // ---------- tool: render_url ----------
    // 全功能渲染器：任意 URL，任意输出组合。用于 browser_search/get_web_content 覆盖不到的
    // 场景（需要 PDF/截图/原始 HTML、或手动指定搜索引擎 URL）。
    mcp::tool renderTool = mcp::tool_builder("render_url")
        .with_description(
            "Open ANY http(s) URL in a real headless Chromium browser (Qt "
            "WebEngine), execute JavaScript, wait for dynamic content (SPAs, "
            "ajax), and return the fully-rendered page. Unlimited access to any "
            "URL, any number of times.\n"
            "Tool selection:\n"
            "  - To find information by keyword: use browser_search (it builds the "
            "search-engine URL for you).\n"
            "  - To read one article/page as clean text: use get_web_content (it "
            "auto-strips navigation/ads for the article body).\n"
            "  - Use render_url directly when you need PDF, screenshot, raw "
            "HTML, conservative markdown, a custom settle/timeout, or want to "
            "drive a specific search-engine URL yourself.\n"
            "Default returns clean markdown; output may be html, pdf, "
            "screenshot, or a comma combination. Screenshots come back as image "
            "content; PDFs as base64 text.")
        .with_string_param("url", "The http(s) URL to render")
        .with_string_param("output",
            "Output format(s), comma-separated. Options: 'markdown' (default, "
            "clean readable text), 'html' (raw rendered HTML), 'pdf' (page as "
            "PDF, returned base64), 'screenshot' (full-page image). "
            "Combinations allowed, e.g. 'markdown,screenshot'. Default markdown.",
            false)
        .with_string_param("md_algorithm",
            "Markdown extraction algorithm: 'conservative' (default, zero "
            "false-positive, keeps full DOM) or 'readability' (article-focused, "
            "strips non-content). Only affects markdown output.",
            false)
        .with_string_param("format",
            "Screenshot encoding: 'auto' (default, smart pick by image ratio), "
            "'png' (lossless), 'jpg' (smaller for photos). Only affects screenshot.",
            false)
        .with_number_param("settle_ms",
            "Milliseconds to wait after load for JS/ajax to finish (default 2500)",
            false)
        .with_number_param("timeout_ms",
            "Max milliseconds to wait for render (default 45000; raise to 60000 for slow/blocked sites)", false)
        .build();

    m_impl->srv->register_tool(renderTool,
        [impl](const mcp::json& params, const std::string&) -> mcp::json {
            std::string url = params.value("url", "");
            if (url.empty()) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                                         "'url' is required");
            }
            // 解析 output：默认 markdown。识别 html/markdown/md/pdf/screenshot/image/png。
            // 小写化后按子串匹配（与 /render 的 parseOutputs 宽容策略一致）。
            std::string outputRaw = params.value("output", "markdown");
            std::string ol;
            for (char c : outputRaw) ol += char(std::tolower(unsigned(c)));
            bool wantHtml = ol.find("html") != std::string::npos;
            bool wantMd = ol.find("markdown") != std::string::npos || ol.find("md") != std::string::npos;
            bool wantPdf = ol.find("pdf") != std::string::npos;
            bool wantShot = ol.find("screenshot") != std::string::npos
                          || ol.find("image") != std::string::npos
                          || ol.find("png") != std::string::npos;
            // 默认 markdown（当未显式指定任何文本输出时）
            if (!wantHtml && !wantMd) wantMd = true;

            int settle = params.value("settle_ms", 2500);
            int timeout = params.value("timeout_ms", 45000);
            std::string mdAlg = params.value("md_algorithm", "conservative");
            std::string fmt = params.value("format", "auto");

            // 组装 /render 的 output 规格（逗号分隔，与 HTTP 接口一致）
            std::string outSpec;
            auto addSep = [&]() { if (!outSpec.empty()) outSpec += ","; };
            if (wantHtml) { addSep(); outSpec += "html"; }
            if (wantMd)   { addSep(); outSpec += "markdown"; }
            if (wantPdf)  { addSep(); outSpec += "pdf"; }
            if (wantShot) { addSep(); outSpec += "screenshot"; }

            mcp::json body = {
                {"url", url},
                {"output", outSpec},
                {"settle_ms", settle},
                {"long_poll_ms", timeout},
                {"md_algorithm", mdAlg},
                {"format", fmt},
            };
            mcpLog("render_url url=%s output=%s settle_ms=%d timeout_ms=%d",
                   url.c_str(), outSpec.c_str(), settle, timeout);
            std::string resp = impl->httpPost("/render", body.dump());
            if (resp.empty()) {
                mcpLog("render_url url=%s -> render service unreachable", url.c_str());
                return toTextContent("error: render service unreachable");
            }

            try {
                auto j = mcp::json::parse(resp);
                std::string state = j.value("state", "unknown");
                std::string task_id = j.value("task_id", "");

                // running：任务仍在进行（long_poll_ms 不够）。提示 agent 可补取。
                if (state == "running") {
                    mcpLog("render_url url=%s -> running (still rendering) task_id=%s",
                           url.c_str(), task_id.c_str());
                    return toTextContent(
                        "Render is still running (task_id=" + task_id + "). "
                        "It did not finish within the wait window. "
                        "Call get_render_result with this task_id to poll for completion.");
                }

                // 非成功：给出可操作的诊断 + 建议，而非枯燥的错误码
                if (state != "succeeded") {
                    std::string err = j.value("error", "");
                    mcpLog("render_url url=%s -> FAILED task_id=%s state=%s error=%s",
                           url.c_str(), task_id.c_str(), state.c_str(), err.c_str());
                    std::string hint = "state=" + state + " error=" + err + " url=" + url;
                    // 超时：常因反爬风控（TLS RST）或慢站点。建议提高超时 + 稍后重试。
                    if (err.find("timed out") != std::string::npos) {
                        hint += "\n\nThis is often caused by transient anti-bot rate-limiting "
                                "(especially Google/Bing) or a slow site. Suggestions:\n"
                                "1. Retry after a few seconds (rate limits usually cool down).\n"
                                "2. Increase timeout_ms (e.g. 60000) for slow pages.\n"
                                "3. For Google Search, avoid rapid consecutive queries from the same proxy.\n"
                                "4. Try an alternative search engine (Bing, DuckDuckGo) if Google keeps blocking.";
                    }
                    return toTextContent(hint);
                }

                // 成功：组装 content 数组（text + 可选 image）。
                // 文本部分：优先 markdown > html；含元信息头。
                mcpLog("render_url url=%s -> SUCCEEDED task_id=%s output=%s"
                       " (md=%zuB html=%zuB pdf=%s screenshot=%s)",
                       url.c_str(), task_id.c_str(), outSpec.c_str(),
                       wantMd ? j.value("markdown", std::string()).size() : 0,
                       wantHtml ? j.value("html", std::string()).size() : 0,
                       (wantPdf && j.value("has_pdf", false)) ? "yes" : "no",
                       (wantShot && j.value("has_image", false)) ? "yes" : "no");
                mcp::json contents = mcp::json::array();
                std::string textPart;
                textPart += "task_id=" + task_id + " state=" + state + " url=" + url + "\n\n";
                if (wantMd) {
                    std::string md = j.value("markdown", std::string());
                    if (j.contains("md_algorithm_used")) {
                        textPart += "[md_algorithm: " + j.value("md_algorithm_used", std::string()) + "]\n\n";
                    }
                    textPart += md.empty() ? "(markdown empty)" : md;
                } else if (wantHtml) {
                    std::string html = j.value("html", std::string());
                    textPart += html.empty() ? "(html empty)" : html;
                }
                // pdf/screenshot 的元信息附在文本末尾
                if (wantPdf && j.value("has_pdf", false)) {
                    textPart += "\n\n[pdf: " + std::to_string(j.value("pdf_bytes", 0)) + " bytes]";
                }
                if (wantShot && j.value("has_image", false)) {
                    textPart += "\n\n[screenshot: " + std::to_string(j.value("image_bytes", 0))
                              + " bytes, " + j.value("image_format", std::string()) + "]";
                    if (j.value("image_truncated", false)) textPart += " (truncated)";
                }
                contents.push_back({ {"type", "text"}, {"text", textPart} });

                // 截图：拉取二进制并作为 image content 返回（MCP 原生图片，agent 可直接显示）
                if (wantShot && j.value("has_image", false)) {
                    std::string imgPath = j.value("image", std::string()); // 形如 "/image/<id>"
                    if (!imgPath.empty()) {
                        std::string imgBytes = impl->httpGet(imgPath);
                        if (!imgBytes.empty()) {
                            std::string mime = (j.value("image_format", std::string()) == "jpeg")
                                             ? "image/jpeg" : "image/png";
                            contents.push_back(toImageContent(base64Encode(imgBytes), mime));
                        }
                    }
                }
                // PDF：MCP 无 PDF 原生类型，作为 base64 text 返回（agent 可解码保存）
                if (wantPdf && j.value("has_pdf", false)) {
                    std::string pdfPath = j.value("pdf", std::string()); // 形如 "/pdf/<id>"
                    if (!pdfPath.empty()) {
                        std::string pdfBytes = impl->httpGet(pdfPath);
                        if (!pdfBytes.empty()) {
                            contents.push_back({ {"type", "text"},
                                {"text", "[PDF base64 below, " + std::to_string(pdfBytes.size())
                                 + " bytes — decode to save as .pdf]\n" + base64Encode(pdfBytes)} });
                        }
                    }
                }
                return contents;
            } catch (...) {
                return toTextContent(resp);
            }
        });

    // ---------- tool: get_render_result ----------
    // 按已有 task_id 拉取结果（用于 render_url 超时后补取，或跨会话）。
    mcp::tool resultTool = mcp::tool_builder("get_render_result")
        .with_description(
            "Fetch the result of a previously submitted render_url task by "
            "task_id. Use when render_url returned state=running (still loading, "
            "e.g. a slow search engine or heavy page) to poll for completion, "
            "or to re-pull a finished result. Default returns markdown; set "
            "output=\"html\" for raw rendered HTML, or \"screenshot\"/\"pdf\" "
            "to fetch binary outputs produced by the original render.")
        .with_string_param("task_id", "The task id returned by render_url")
        .with_string_param("output",
            "Output format to fetch: 'markdown' (default), 'html', 'screenshot', "
            "or 'pdf'. Use the same or a subset of what render_url requested.",
            false)
        .with_number_param("timeout_ms",
            "Max milliseconds to wait if not finished yet (default 5000)", false)
        .build();

    m_impl->srv->register_tool(resultTool,
        [impl](const mcp::json& params, const std::string&) -> mcp::json {
            std::string task_id = params.value("task_id", "");
            if (task_id.empty()) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                                         "'task_id' is required");
            }
            std::string outputRaw = params.value("output", "markdown");
            std::string ol;
            for (char c : outputRaw) ol += char(std::tolower(unsigned(c)));
            // /result/:id 的 output 单值：优先级 screenshot > pdf > html > markdown
            std::string apiOutput;
            bool wantShot = ol.find("screenshot") != std::string::npos
                          || ol.find("image") != std::string::npos;
            bool wantPdf = ol.find("pdf") != std::string::npos;
            bool wantHtml = ol.find("html") != std::string::npos;
            if (wantShot) apiOutput = "screenshot";
            else if (wantPdf) apiOutput = "pdf";
            else if (wantHtml) apiOutput = "html";
            else apiOutput = "markdown";

            int timeout = params.value("timeout_ms", 5000);
            std::string path = "/result/" + task_id +
                               "?output=" + apiOutput +
                               "&timeout=" + std::to_string(timeout);
            mcpLog("get_render_result task_id=%s output=%s timeout_ms=%d",
                   task_id.c_str(), apiOutput.c_str(), timeout);
            std::string resp = impl->httpGet(path);
            if (resp.empty()) {
                mcpLog("get_render_result task_id=%s -> render service unreachable",
                       task_id.c_str());
                return toTextContent("error: render service unreachable");
            }

            // 文本输出直接返回
            if (!wantShot && !wantPdf) {
                // 文本分支响应体即为最终内容（非 JSON）。无法可靠解析 state，
                // 只记录返回字节数即可。
                mcpLog("get_render_result task_id=%s -> returned %zu bytes (text)",
                       task_id.c_str(), resp.size());
                return toTextContent(resp);
            }

            // 二进制输出：解析 JSON 中的路径引用，再拉取并 base64
            try {
                auto j = mcp::json::parse(resp);
                std::string state = j.value("state", "unknown");
                if (state != "succeeded") {
                    mcpLog("get_render_result task_id=%s -> state=%s (no binary fetched)",
                           task_id.c_str(), state.c_str());
                    return toTextContent(resp);
                }
                mcpLog("get_render_result task_id=%s -> SUCCEEDED state=%s",
                       task_id.c_str(), state.c_str());
                mcp::json contents = mcp::json::array();
                std::string textPart = "task_id=" + task_id + " state=" + state + "\n";
                if (wantShot && j.value("has_image", false)) {
                    textPart += "[screenshot: " + std::to_string(j.value("image_bytes", 0))
                              + " bytes, " + j.value("image_format", std::string()) + "]";
                    contents.push_back({ {"type", "text"}, {"text", textPart} });
                    std::string imgPath = j.value("image", std::string());
                    if (!imgPath.empty()) {
                        std::string imgBytes = impl->httpGet(imgPath);
                        if (!imgBytes.empty()) {
                            std::string mime = (j.value("image_format", std::string()) == "jpeg")
                                             ? "image/jpeg" : "image/png";
                            contents.push_back(toImageContent(base64Encode(imgBytes), mime));
                        }
                    }
                    return contents;
                }
                if (wantPdf && j.value("has_pdf", false)) {
                    textPart += "[pdf: " + std::to_string(j.value("pdf_bytes", 0)) + " bytes]";
                    contents.push_back({ {"type", "text"}, {"text", textPart} });
                    std::string pdfPath = j.value("pdf", std::string());
                    if (!pdfPath.empty()) {
                        std::string pdfBytes = impl->httpGet(pdfPath);
                        if (!pdfBytes.empty()) {
                            contents.push_back({ {"type", "text"},
                                {"text", "[PDF base64]\n" + base64Encode(pdfBytes)} });
                        }
                    }
                    return contents;
                }
                // 未产出该二进制（原 render 没请求）：回传状态 JSON
                return toTextContent(resp);
            } catch (...) {
                return toTextContent(resp);
            }
        });

    // ---------- tool: browser_search ----------
    // 高层封装：输入关键词 → 自动构造搜索引擎 URL → 渲染 → 返回结果页 markdown。
    // 对 agent 更直接：不用自己拼 ?q= / URL 编码，也明确语义是"找信息"而非"渲染某页"。
    // 引擎默认 Google；Google 反爬较重时 agent 可传 engine=bing/baidu/duckduckgo。
    // 注：工具命名为 browser_search（而非 web_search）以避免与各 agent 框架内置的
    // 同名「web_search/内容检索」工具命名冲突，导致多工具注册时重名或路由歧义。
    mcp::tool searchTool = mcp::tool_builder("browser_search")
        .with_description(
            "Search the web by keyword and return the search-engine result "
            "page (a list of result links with titles and snippets). PREFER "
            "THIS whenever the user wants to find or look up information but "
            "has NOT given a specific URL — natural-language intents like "
            "'search', 'Google it', 'look up', 'find', 'research', "
            "'搜索/搜一下', '查一下/查资料', '调研', '用百度/谷歌/必应搜', "
            "'帮我了解…', '哪些…靠谱/最好'. This tool builds the search-engine "
            "URL for you and URL-encodes the query, so you do NOT need to "
            "construct the URL yourself. NOTE: for engine='baidu', 'bing', or "
            "'google', results are returned as a clean structured list "
            "(ads/promoted results filtered out, each result has title/url/"
            "snippet/source). For other engines the full result-page markdown is "
            "returned (may include ads). After reading the results, call "
            "get_web_content (or render_url) on any result link to open the full "
            "article.")
        .with_string_param("query",
            "The search query (plain text, NOT URL-encoded). Be specific and "
            "concrete; for best results put the most important terms first.")
        .with_string_param("engine",
            "Search engine to use: 'google' (default), 'bing', 'baidu', or "
            "'duckduckgo'. Google has the best results but is the most "
            "aggressive at anti-bot blocking; if it times out or fails, switch "
            "to 'bing' or 'baidu'. For Chinese-language queries 'baidu' often "
            "works best.",
            false)
        .with_number_param("settle_ms",
            "Milliseconds to wait after load for JS/ajax (default 2500; raise "
            "for slow result pages)",
            false)
        .with_number_param("timeout_ms",
            "Max milliseconds to wait (default 45000; raise to 60000 for "
            "slow/blocked engines)", false)
        .build();

    m_impl->srv->register_tool(searchTool,
        [impl](const mcp::json& params, const std::string&) -> mcp::json {
            std::string query = params.value("query", "");
            if (query.empty()) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                                         "'query' is required");
            }
            std::string engine = params.value("engine", "google");
            std::string el;
            for (char c : engine) el += char(std::tolower(unsigned(c)));
            std::string url;
            if (el.find("baidu") != std::string::npos) {
                url = "https://www.baidu.com/s?wd=" + urlEncode(query);
            } else if (el.find("bing") != std::string::npos) {
                url = "https://www.bing.com/search?q=" + urlEncode(query);
            } else if (el.find("duck") != std::string::npos) {
                url = "https://duckduckgo.com/?q=" + urlEncode(query);
            } else {
                // 默认 google；未识别值也回退 google（宽容）
                url = "https://www.google.com/search?q=" + urlEncode(query);
            }

            int settle = params.value("settle_ms", 2500);
            int timeout = params.value("timeout_ms", 45000);
            bool isBaidu = el.find("baidu") != std::string::npos;
            bool isBing = el.find("bing") != std::string::npos;
            bool isGoogle = el.find("google") != std::string::npos;
            bool structured = isBaidu || isBing || isGoogle;  // 支持结构化提取的引擎
            std::string extractVal;               // 引擎对应的 extract 值
            if (isBaidu) extractVal = "baidu_serp";
            else if (isBing) extractVal = "bing_serp";
            else if (isGoogle) extractVal = "google_serp";
            mcp::json body = {
                {"url", url},
                {"settle_ms", settle},
                {"long_poll_ms", timeout},
            };
            if (structured) {
                // 结构化引擎：走 SERP 提取（去广告，输出 serp_json），不转 markdown（SERP 页 markdown 是噪声）。
                body["output"] = "html";
                body["extract"] = extractVal;
            } else {
                // ddg：维持现状（整页 conservative markdown）。
                body["output"] = "markdown";
                body["md_algorithm"] = "conservative";
            }
            mcpLog("browser_search engine=%s query=\"%s\" -> url=%s settle_ms=%d timeout_ms=%d",
                   engine.c_str(), query.c_str(), url.c_str(), settle, timeout);
            std::string resp = impl->httpPost("/render", body.dump());
            if (resp.empty()) {
                mcpLog("browser_search engine=%s query=\"%s\" -> render service unreachable",
                       engine.c_str(), query.c_str());
                return toTextContent("error: render service unreachable");
            }
            try {
                auto j = mcp::json::parse(resp);
                std::string state = j.value("state", "unknown");
                std::string task_id = j.value("task_id", "");
                if (state == "running") {
                    mcpLog("browser_search engine=%s query=\"%s\" -> running (still loading) task_id=%s",
                           engine.c_str(), query.c_str(), task_id.c_str());
                    return toTextContent(
                        "Search is still running (task_id=" + task_id + ", "
                        "engine=" + engine + ", query=\"" + query + "\"). "
                        "Call get_render_result with this task_id to poll for "
                        "completion.");
                }
                if (state != "succeeded") {
                    std::string err = j.value("error", "");
                    mcpLog("browser_search engine=%s query=\"%s\" -> FAILED task_id=%s state=%s error=%s",
                           engine.c_str(), query.c_str(), task_id.c_str(), state.c_str(), err.c_str());
                    std::string hint = "Search failed: state=" + state +
                                       " engine=" + engine +
                                       " query=\"" + query + "\" error=" + err;
                    if (err.find("timed out") != std::string::npos) {
                        hint += "\n\nSearch engines (especially Google) often "
                                "rate-limit or block automated queries. "
                                "Suggestions:\n"
                                "1. Retry with engine='bing' or 'baidu' (less "
                                "aggressive blocking).\n"
                                "2. For Chinese queries, engine='baidu' usually "
                                "works best.\n"
                                "3. Increase timeout_ms (e.g. 60000).\n"
                                "4. Retry after a few seconds.";
                    }
                    return toTextContent(hint);
                }
                // 结构化引擎（baidu/bing）：消费 serp_json 组装结构化输出。
                if (structured) {
                    if (!j.contains("serp_json")) {
                        // 提取未生效（JS 缺失/异常）→ 降级返回 raw html 前 2000 字符诊断。
                        mcpLog("browser_search engine=%s query=\"%s\" -> SUCCEEDED but serp_json missing "
                               "(extraction unavailable, degraded to raw html)",
                               engine.c_str(), query.c_str());
                        std::string htmlRaw = j.value("html", std::string());
                        std::string text = "[browser_search engine=" + engine + " query=\"" + query + "\"]\n"
                                           "(extraction unavailable; raw html preview)\n\n" +
                                           htmlRaw.substr(0, 2000);
                        return toTextContent(text);
                    }
                    auto sj = j["serp_json"];
                    if (sj.value("blocked", false)) {
                        mcpLog("browser_search engine=%s query=\"%s\" -> BLOCKED by search engine (task_id=%s)",
                               engine.c_str(), query.c_str(), task_id.c_str());
                        return toTextContent(
                            "[browser_search engine=" + engine + " query=\"" + query + "\"]\n"
                            "BLOCKED: the search engine returned a verification/block page. "
                            "Suggestions:\n"
                            "1. Retry after a few seconds.\n"
                            "2. Try a different engine (engine='baidu'/'bing').\n"
                            "3. Increase timeout_ms (e.g. 60000).");
                    }
                    auto results = sj.value("results", mcp::json::array());
                    if (results.empty()) {
                        mcpLog("browser_search engine=%s query=\"%s\" -> SUCCEEDED but no results (task_id=%s)",
                               engine.c_str(), query.c_str(), task_id.c_str());
                        return toTextContent(
                            "[browser_search engine=" + engine + " query=\"" + query + "\"]\n"
                            "(no results found)");
                    }
                    // 组装：JSON 块（精确可解析）+ 可读编号列表。
                    mcpLog("browser_search engine=%s query=\"%s\" -> SUCCEEDED task_id=%s results=%zu",
                           engine.c_str(), query.c_str(), task_id.c_str(), results.size());
                    std::string text = "[browser_search engine=" + engine + " query=\"" + query +
                                       "\" count=" + std::to_string(results.size()) + "]\n\n";
                    text += "```json\n" + results.dump(2) + "\n```\n\n";
                    text += "Results:\n";
                    int idx = 1;
                    for (auto& r : results) {
                        text += std::to_string(idx++) + ". **" + r.value("title", std::string()) + "**";
                        std::string src = r.value("source", std::string());
                        if (!src.empty()) text += " — " + src;
                        text += "\n   " + r.value("url", std::string());
                        if (r.value("is_redirect", false)) text += "  (redirect link)";
                        std::string snip = r.value("snippet", std::string());
                        if (!snip.empty()) text += "\n   " + snip;
                        text += "\n";
                        std::string rtype = r.value("type", std::string());
                        if (rtype != "organic")
                            text += "   _[" + rtype + "]_\n";
                    }
                    auto rec = sj.value("recommend", mcp::json::array());
                    if (!rec.empty()) {
                        text += "\nRelated searches: ";
                        for (size_t i = 0; i < rec.size(); ++i)
                            text += (i ? ", " : "") + rec[i].get<std::string>();
                        text += "\n";
                    }
                    return toTextContent(text);
                }
                // 非 baidu：维持现有 markdown 返回。
                std::string md = j.value("markdown", std::string());
                mcpLog("browser_search engine=%s query=\"%s\" -> SUCCEEDED task_id=%s (markdown %zu bytes)",
                       engine.c_str(), query.c_str(), task_id.c_str(), md.size());
                std::string text = "[browser_search engine=" + engine +
                                   " query=\"" + query + "\"]\n\n";
                text += md.empty() ? "(no results / empty page)" : md;
                return toTextContent(text);
            } catch (...) {
                return toTextContent(resp);
            }
        });

    // ---------- tool: get_web_content ----------
    // 高层封装：打开单个 URL → 用 readability 提取正文（去导航/广告/侧栏）。
    // 与 render_url 区分：render_url 默认 conservative（全 DOM），get_web_content 默认
    // readability（仅正文），语义上就是"读这篇文章给我看"。
    // 注：工具命名为 get_web_content（而非 web_reader）以避免与各 agent 框架内置的
    // 同名「web_reader/网页读取」工具命名冲突。
    mcp::tool readerTool = mcp::tool_builder("get_web_content")
        .with_description(
            "Open a single article/page URL and extract its main content as "
            "clean markdown (article body, with navigation/ads/sidebars "
            "stripped via the readability algorithm). PREFER THIS when the user "
            "gives a specific URL to read, or after browser_search returns result "
            "links that you want to open in full. For pages that are NOT "
            "articles (e.g. tables, dashboards, where you need the full DOM), "
            "use render_url with md_algorithm='conservative' instead.")
        .with_string_param("url", "The http(s) URL of the article/page to read")
        .with_string_param("md_algorithm",
            "Markdown extraction: 'readability' (default, article body only — "
            "best for news/blogs/papers) or 'conservative' (full DOM, zero "
            "false-positive — use when readability drops content you need).",
            false)
        .with_number_param("settle_ms",
            "Milliseconds to wait after load for JS/ajax (default 2500)",
            false)
        .with_number_param("timeout_ms",
            "Max milliseconds to wait (default 45000; raise to 60000 for slow "
            "sites)", false)
        .build();

    m_impl->srv->register_tool(readerTool,
        [impl](const mcp::json& params, const std::string&) -> mcp::json {
            std::string url = params.value("url", "");
            if (url.empty()) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                                         "'url' is required");
            }
            std::string mdAlg = params.value("md_algorithm", "readability");
            int settle = params.value("settle_ms", 2500);
            int timeout = params.value("timeout_ms", 45000);
            mcp::json body = {
                {"url", url},
                {"output", "markdown"},
                {"settle_ms", settle},
                {"long_poll_ms", timeout},
                {"md_algorithm", mdAlg},
            };
            mcpLog("get_web_content url=%s md_algorithm=%s settle_ms=%d timeout_ms=%d",
                   url.c_str(), mdAlg.c_str(), settle, timeout);
            std::string resp = impl->httpPost("/render", body.dump());
            if (resp.empty()) {
                mcpLog("get_web_content url=%s -> render service unreachable", url.c_str());
                return toTextContent("error: render service unreachable");
            }
            try {
                auto j = mcp::json::parse(resp);
                std::string state = j.value("state", "unknown");
                std::string task_id = j.value("task_id", "");
                if (state == "running") {
                    mcpLog("get_web_content url=%s -> running (still loading) task_id=%s",
                           url.c_str(), task_id.c_str());
                    return toTextContent(
                        "Read is still running (task_id=" + task_id + "). "
                        "Call get_render_result with this task_id to poll for "
                        "completion.");
                }
                if (state != "succeeded") {
                    std::string err = j.value("error", "");
                    mcpLog("get_web_content url=%s -> FAILED task_id=%s state=%s error=%s",
                           url.c_str(), task_id.c_str(), state.c_str(), err.c_str());
                    return toTextContent("Read failed: state=" + state +
                                         " error=" + err + " url=" + url);
                }
                std::string md = j.value("markdown", std::string());
                mcpLog("get_web_content url=%s -> SUCCEEDED task_id=%s markdown=%zu bytes",
                       url.c_str(), task_id.c_str(), md.size());
                std::string text = "[get_web_content url=" + url;
                if (j.contains("md_algorithm_used")) {
                    text += " md=" + j.value("md_algorithm_used", std::string());
                }
                text += "]\n\n";
                text += md.empty() ? "(empty content — the page may be non-article; "
                                   "retry render_url with md_algorithm='conservative')"
                                   : md;
                return toTextContent(text);
            } catch (...) {
                return toTextContent(resp);
            }
        });

    // 非阻塞模式启动。关键：会话清理线程（每 10s 回收过期 session）只在非阻塞模式下启动。
    // 阻塞模式下 session 只增不减，连满 max_sessions 后新连接永久 503（这正是 /mcp 反复重连
    // 后掉线的根因）。非阻塞模式由 cpp-mcp 自起 server 与 maintenance 线程。
    m_impl->running = true;
    if (!m_impl->srv->start(false)) {
        std::fprintf(stderr,
            "WARNING: MCP server failed to listen on %s:%d "
            "(HTTP/WS rendering still works)\n",
            m_impl->mcpHost.c_str(), m_impl->mcpPort);
        m_impl->running = false;
    }
    m_impl->thr = {};  // 非阻塞模式下 cpp-mcp 自管线程，不再需要我们的保活线程

    return true;
}

void McpServer::stop() {
    if (m_impl->srv) {
        m_impl->srv->stop();
    }
    if (m_impl->thr.joinable()) m_impl->thr.join();
    m_impl->running = false;
}

} // namespace seimi
