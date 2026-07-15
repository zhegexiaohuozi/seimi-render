// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "RenderQueue.h"

#include "IpNet.h"

#include <QHash>
#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string>

// 前置：httplib 是单头文件库，在 .cpp 内 include 以隔离其宏污染。
namespace httplib { class Server; class Request; class Response; }

namespace seimi {

class CookieStore;
class ProxyConfig;
class Environment;

// HTTP 接口层（cpp-httplib，内置线程池）。
// 路由：POST /render、GET /status/:id、GET /result/:id?timeout=、GET /stats、GET /health、
// GET/POST/DELETE /cookies、GET/POST/DELETE /proxy、GET /（管理 UI）、POST /api/login。
// handler 跑在 httplib 工作线程，仅经线程安全的 RenderQueue 交互，绝不碰 WebEngine/WS。
// 长轮询通过 RenderQueue::waitForCompletion 阻塞当前工作线程。
class HttpServer : public QObject {
    Q_OBJECT
public:
    HttpServer(RenderQueue* queue, int settleDefaultMs, QObject* parent = nullptr);
    HttpServer(RenderQueue* queue, CookieStore* cookies, int settleDefaultMs, QObject* parent = nullptr);
    // 含管理 UI 与可选密码：adminUiDir 为 admin-ui 资源目录绝对路径（空=不启用管理页）；
    // password 非空时启用 token 登录保护（除 /health、/api/login 外所有路径都需 token）。
    HttpServer(RenderQueue* queue, CookieStore* cookies, int settleDefaultMs,
               std::string adminUiDir, std::string password,
               QObject* parent = nullptr);
    ~HttpServer() override;

    // 在独立线程启动 httplib server（阻塞监听）。返回是否成功 listen。
    bool start(const std::string& host, int port, int threadCount);

    void stop();

    // 由 password 确定性派生 token（sha256）。公开静态方法：main.cpp 用它算同一 token
    // 传给 McpServer（MCP 工具调渲染 API 时带此 token 鉴权）。password 空→返回空。
    static std::string computeToken(const std::string& password);

    // 设置可信反向代理网段（逗号分隔，精确 IP 与 CIDR）。非空时 /api/login 从 XFF 还原真实
    // 客户端 IP（仅信任直连来源命中的可信代理）。为空则用 TCP 直连地址，忽略 XFF。
    // 必须在 start() 前调用。
    void setTrustedProxies(const std::string& csv);

    // 设置网络代理配置中枢（启用 GET/POST/DELETE /proxy）。必须在 start() 前调用。
    void setProxyConfig(ProxyConfig* cfg) { m_proxy = cfg; }
    void setEnvironment(Environment* env) { m_env = env; }

private:
    void registerRoutes();

    // JSON 构造辅助。outMask 为本次请求想要的输出位标记，决定回传 html/markdown/image 元信息。
    // 入参用持锁快照 Snapshot（而非裸 RenderTaskPtr），避免跨线程读可变字段的数据竞争。
    static std::string jsonStateResp(const RenderQueue::Snapshot& s, OutputMask outMask);

    // GET /status 的运行时全景 JSON：进程信息 + 累计计数 + 成功率 + 延迟分布 +
    // 吞吐 + 输出类型需求 + 域名分布(top-N) + 当前队列快照 + 当前网络代理生效配置。
    std::string jsonRuntimeStatus(const httplib::Request& req);

    // —— 管理 UI 与密码保护 ——
    // 读取 adminUiDir/index.html 全文（首次调用缓存）。失败返回空。
    std::string loadAdminIndex();
    // 启用密码时，校验请求里的 token（Authorization: Bearer 或 query ?token=）。
    bool checkAuth(const httplib::Request& req) const;
    // 未通过鉴权时的统一 401 响应。
    static void rejectAuth(httplib::Response& res);

    // —— /api/login 按 IP 限流 ——
    // 单密码模型下，全局锁会让攻击者与管理员混在同一个锁里（锁内校验密码→对在线暴力
    // 形同虚设；锁内不校验→攻击者触发后管理员被一起锁死）。按 IP 隔离即可两全：
    // 攻击者 IP 连续失败→该 IP 冻结（锁内不校验，在线暴力被掐断）；管理员 IP 不在锁内→正常登录。
    //
    // 客户端 IP 取 req.remote_addr；仅配置过可信代理时才用 XFF 覆盖（否则伪造 XFF 无效）。
    struct LoginRate {
        int failures{0};       // 该 IP 当前连续失败次数（成功即清零）
        qint64 lockUntilMs{0}; // 锁定截止时刻；now < 此值则该 IP 被冻结
    };
    // 记录某 IP 一次登录失败：失败计数 +1，达上限则锁定 kLoginLockMs。
    void noteLoginFailure(const QString& ip);
    // 记录某 IP 一次登录成功：清零该 IP 的计数与锁定。
    void noteLoginSuccess(const QString& ip);
    // 某 IP 当前是否处于锁定窗口；是则返回剩余锁定毫秒（>0），否则返回 0。
    qint64 loginLockRemainingMs(const QString& ip) const;
    // 清理过期（锁定已到期且失败计数为 0）的 IP 条目，防哈希表随时间无限增长。
    void gcLoginRate();
    // 限流表达 kMaxLoginIps 上限时牺牲最陈旧条目（lockUntilMs 最小者）腾槽。持锁调用，
    // 防 --trusted-proxy 下伪造 XFF 注入无界 IP 条目耗尽内存。
    void reclaimLoginSlotLocked();

    RenderQueue* m_queue;
    CookieStore* m_cookies = nullptr;  // 可空；非空时启用 /cookies 接口
    ProxyConfig* m_proxy = nullptr;    // 可空；非空时启用 /proxy 接口
    Environment* m_env = nullptr;      // 可空；非空时 /status 附带 environment 段
    int m_settleDefaultMs;
    std::unique_ptr<httplib::Server> m_srv;
    std::atomic<bool> m_running{false};

    std::string m_adminUiDir;          // admin-ui 资源目录（空=不启用管理页）
    std::string m_password;            // 非空=启用密码保护
    std::string m_token;               // 启用密码时，登录成功返回的固定 token
    std::string m_adminIndexCache;     // index.html 内容缓存
    bool m_adminIndexLoaded = false;

    // 可信反向代理网段。非空时 /api/login 用 X-Forwarded-For 还原真实客户端 IP 做限流。
    std::vector<IpNet> m_trustedProxies;

    // 按 IP 的登录限流状态。qint64 用 epoch ms。
    // 策略：每 IP 连续失败累计，达 kLoginMaxFailures 次后锁定 kLoginLockMs。
    mutable QMutex m_loginMutex;
    QHash<QString, LoginRate> m_loginByIp;
    qint64 m_lastLoginGcMs{0};         // 上次 gc 时间，节流清理频率
    static constexpr int kLoginMaxFailures = 10;
    static constexpr qint64 kLoginLockMs = 10 * 60 * 1000;  // 10 分钟
    static constexpr qint64 kLoginGcIntervalMs = 5 * 60 * 1000; // gc 节流：5 分钟
    static constexpr int kMaxLoginIps = 10000;  // 按 IP 限流表硬上限（防伪造 XFF 无界增长）
};

} // namespace seimi
