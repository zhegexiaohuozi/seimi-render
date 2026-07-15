#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace seimi {

// MCP (Model Context Protocol) 服务入口。
// 把 seimi-render 渲染能力以 MCP tools 暴露给 Claude Code、Cursor 等 agent 工具。
// MCP server 在本进程独立端口监听（默认 8090），工具内部经 HTTP 调本进程渲染 API，
// 不直接碰 WebEngine。
//
// 纯 C++ 类（不依赖 QObject），便于独立测试/复用。
class McpServer {
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // 启动 MCP HTTP server（在独立线程阻塞监听）。host/port 为本 MCP 端点；
    // renderHost/renderPort 为本进程渲染 API（MCP 工具会调它）。
    // authToken 非空时，工具内部调渲染 API 会带 Authorization: Bearer <token>
    //（仅当 seimi-render 启用了 --password 时需要，否则传空）。
    bool start(const std::string& host, int port,
               const std::string& renderHost, int renderPort,
               const std::string& authToken = std::string());

    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace seimi
