#pragma once

#include <QMutex>
#include <QMutexLocker>

#include <cstdint>
#include <string>

class QNetworkProxy;

namespace seimi {

// 上游代理配置。Direct = 不经代理、直连目标；Http/Socks5 = 走指定代理。
struct ProxySpec {
    enum class Type { Direct, Http, Socks5 };

    Type type = Type::Direct;
    std::string host;   // Http/Socks5 必填
    quint16 port = 0;   // Http/Socks5 必填
    std::string user;   // 可空（代理认证用户名）
    std::string pass;   // 可空（代理认证密码）

    bool operator==(const ProxySpec& o) const {
        return type == o.type && host == o.host && port == o.port
            && user == o.user && pass == o.pass;
    }
    bool operator!=(const ProxySpec& o) const { return !(*this == o); }
};

// 网络代理配置中枢：HTTP 线程写、GUI 线程 apply。
// setProxy()（HTTP 线程）加锁存值 + 标 dirty；apply()（GUI 线程，RenderPool 定时器驱动）
// 若 dirty 则调 QNetworkProxy::setApplicationProxy。
// Qt WebEngine 轮询全局应用代理，运行时修改后 Chromium 在数秒内切换。
// 替代了运行时不可变的 --proxy-server 启动 flag。
class ProxyConfig {
public:
    ProxyConfig() = default;

    // HTTP 线程：更新代理配置。加锁存值 + 标 dirty。立即返回，不触碰 Qt。
    void setProxy(const ProxySpec& spec) {
        QMutexLocker lock(&m_mutex);
        m_pending = spec;
        m_dirty = true;
    }

    // GUI 线程：若有变更，把 pending 写入 QNetworkProxy::setApplicationProxy。
    // 首次 apply（启动初始代理）也会执行。幂等：无变更时直接返回。
    void apply();

    // 任意线程（GET /proxy）：取当前配置快照（用户最近 setProxy 的值，含密码，
    // 调用方按需脱敏）。读 pending（而非已 apply 的），让 POST 后立即 GET 能看到新值。
    ProxySpec snapshot() const {
        QMutexLocker lock(&m_mutex);
        return m_pending;
    }

private:
    mutable QMutex m_mutex;
    ProxySpec m_pending;   // setProxy 写入；apply 与 snapshot 都读它
    bool m_dirty = false;  // 有待 apply 的变更（apply 成功后清）
};

} // namespace seimi
