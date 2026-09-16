// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

class QNetworkProxy;
class QNetworkProxyFactory;

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
// 按 bypass 配置二选一：
//   - 未配置 bypass（默认）：QNetworkProxy::setApplicationProxy 全局单值（历史行为不变）。
//   - 配置了 --proxy-bypass：不触碰任何全局 Qt 代理状态（apply 只更新内部生效值）。
//     渲染侧分流由 main.cpp 注入 Chromium 原生 --proxy-server/--proxy-bypass-list 实现；
//     /fetch 侧由 /fetch 专属 QNAM 挂载 fetchProxyFactory()（实例级 setProxyFactory）实现。
// ⚠️ 为什么不能注册全局 QNetworkProxyFactory（2026-09-15 实测教训）：全局 factory 一旦
// 注册，QNetworkProxy::applicationProxy() 就变成 factory 对「空 host 查询」的返回值——
// QtWebEngine 的 ProxyConfigServiceQt 轮询到它后会把「无 bypass 规则的 fixed 代理配置」
// 推送给 Chromium，**覆盖掉 command line 里的 --proxy-server/--proxy-bypass-list**，
// 渲染分流整个失效。实例级 factory 无此副作用。
// ⚠️ 另一实测事实：QtWebEngine 渲染网络栈不按请求查询 proxy factory（只轮询全局单值），
// per-host 分流只对 QNAM 生效——渲染侧只能靠 Chromium flag，因此 bypass 模式下渲染代理
// 是启动期快照，POST /proxy 热切换只影响 /fetch；这是已知且文档化的限制。
// 非 bypass 模式：Qt WebEngine 轮询全局应用代理，运行时修改后 Chromium 数秒内切换。
class ProxyConfig {
public:
    ProxyConfig() = default;
    ~ProxyConfig();

    // HTTP 线程：更新代理配置。加锁存值 + 标 dirty。立即返回，不触碰 Qt。
    void setProxy(const ProxySpec& spec) {
        QMutexLocker lock(&m_mutex);
        m_pending = spec;
        m_dirty = true;
    }

    // GUI 线程（main 启动编排）：配置 bypass 列表，逗号分隔。每项为域名（后缀匹配，
    // gwm.cn 命中 jira.gwm.cn）/ 精确主机名 / IP 字面量 / IPv4 CIDR。
    // 始终隐含 localhost、127.0.0.1、::1（回环绝不该被代理劫持）。必须在首次 apply() 前调用。
    void setBypass(const QString& commaSeparated);

    // 是否启用了 bypass 分流（main 注入 Chromium flag、/fetch 挂实例级 factory 的前提）。
    bool bypassEnabled() const {
        QMutexLocker lock(&m_mutex);
        return !m_bypassEntries.empty();
    }

    // GUI 线程：若有变更，应用到 Qt（非 bypass 模式调 setApplicationProxy；bypass 模式
    // 只更新内部生效值，见类注释）。幂等：无变更时直接返回。
    void apply();

    // 任意线程（GET /proxy）：取当前配置快照（用户最近 setProxy 的值，含密码，
    // 调用方按需脱敏）。读 pending（而非已 apply 的），让 POST 后立即 GET 能看到新值。
    ProxySpec snapshot() const {
        QMutexLocker lock(&m_mutex);
        return m_pending;
    }

    // 任意线程：最近一次「代理值实际发生变化」的时刻（epoch ms；0=从未变过）。
    // 供失败自诊断判断是否处于代理切换过渡期（Chromium 网络栈轮询 + 在途连接残留）。
    qint64 lastAppliedChangeMsec() const {
        QMutexLocker lock(&m_mutex);
        return m_lastAppliedChangeMsec;
    }

    // 任意线程（proxy factory 的 queryProxy）：目标 host 是否命中 bypass。
    // 持锁遍历小列表，低争用；queryProxy 约束不阻塞，锁内只有字符串比较。
    bool isBypassedForHost(const QString& host) const;

    // 任意线程（proxy factory）：已生效的代理值（区别于 snapshot 的 pending 语义）。
    ProxySpec appliedSnapshot() const {
        QMutexLocker lock(&m_mutex);
        return m_applied;
    }

    // /fetch 的 QNAM 专属 proxy factory（bypass 模式下 /fetch 侧 per-host 分流用）。
    // 懒建一次、ProxyConfig 持有所有权（QNetworkAccessManager::setProxyFactory 不接管
    // 所有权，factory 必须比 QNAM 长命——ProxyConfig 与进程同寿，天然满足）。
    // 只挂 /fetch 的 QNAM 实例，绝不 setApplicationProxyFactory 全局注册（见类注释）。
    QNetworkProxyFactory* fetchProxyFactory();

private:
    // bypass 列表条目（setBypass 时解析一次，匹配时零解析开销）。
    struct BypassEntry {
        QString pattern;        // 小写后的原始条目（域名/主机名/IP 字面量）
        bool isCidr = false;    // IPv4 CIDR 条目
        QHostAddress cidrBase;  // isCidr 时的网络基址
        int cidrPrefix = 0;     // isCidr 时的前缀长度
    };

    bool bypassMatchLocked(const QString& hostLower) const;

    mutable QMutex m_mutex;
    ProxySpec m_pending;   // setProxy 写入；apply 与 snapshot 都读它
    ProxySpec m_applied;   // 已生效值（proxy factory 读取；Direct 初始）
    bool m_dirty = false;  // 有待 apply 的变更（apply 成功后清）
    std::vector<BypassEntry> m_bypassEntries;  // --proxy-bypass 解析结果（空=未启用分流）
    QNetworkProxyFactory* m_fetchFactory = nullptr;  // /fetch 专属 factory（懒建，见上）
    qint64 m_lastAppliedChangeMsec = 0;        // 最近一次代理值变化时刻（过渡期诊断）
};

} // namespace seimi
