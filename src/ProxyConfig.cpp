// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "ProxyConfig.h"

#include <QDateTime>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QString>

namespace seimi {

// ProxySpec -> QNetworkProxy 的统一映射（apply 历史路径与 proxy factory 共用）。
static QNetworkProxy toQNetworkProxy(const ProxySpec& spec) {
    QNetworkProxy proxy;
    switch (spec.type) {
        case ProxySpec::Type::Http:
            proxy.setType(QNetworkProxy::HttpProxy);
            proxy.setHostName(QString::fromStdString(spec.host));
            proxy.setPort(spec.port);
            if (!spec.user.empty()) {
                proxy.setUser(QString::fromStdString(spec.user));
                proxy.setPassword(QString::fromStdString(spec.pass));
            }
            break;
        case ProxySpec::Type::Socks5:
            proxy.setType(QNetworkProxy::Socks5Proxy);
            proxy.setHostName(QString::fromStdString(spec.host));
            proxy.setPort(spec.port);
            if (!spec.user.empty()) {
                proxy.setUser(QString::fromStdString(spec.user));
                proxy.setPassword(QString::fromStdString(spec.pass));
            }
            break;
        case ProxySpec::Type::Direct:
            // 显式 NoProxy（与"不设置"等价且明确），不强制覆盖 Qt 默认代理工厂行为。
            proxy.setType(QNetworkProxy::NoProxy);
            break;
    }
    return proxy;
}

// per-host 分流代理工厂：queryProxy 按目标 host 匹配 bypass 列表——命中回 NoProxy
//（直连），否则返回当前代理。**只挂 /fetch 的 QNAM 实例**（setProxyFactory 实例级），
// 绝不 setApplicationProxyFactory 全局注册——全局注册会让 applicationProxy() 变成 factory
// 对空 host 查询的返回值，QtWebEngine 轮询后推送「无 bypass 规则的 fixed 代理」覆盖掉
// Chromium 的 --proxy-server/--proxy-bypass-list flag（2026-09-15 实测撞过）。
// queryProxy 跑在 QNAM 所属线程（htplib 工作线程），只做锁内快照读 + 字符串比较。
class SeimiProxyFactory : public QNetworkProxyFactory {
public:
    explicit SeimiProxyFactory(const ProxyConfig* cfg) : m_cfg(cfg) {}

    QVector<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        if (m_cfg->isBypassedForHost(query.peerHostName().toLower())) {
            return { QNetworkProxy(QNetworkProxy::NoProxy) };
        }
        return { toQNetworkProxy(m_cfg->appliedSnapshot()) };
    }

private:
    const ProxyConfig* m_cfg;
};

ProxyConfig::~ProxyConfig() {
    delete m_fetchFactory;
}

QNetworkProxyFactory* ProxyConfig::fetchProxyFactory() {
    QMutexLocker lock(&m_mutex);
    if (!m_fetchFactory) m_fetchFactory = new SeimiProxyFactory(this);
    return m_fetchFactory;
}

void ProxyConfig::setBypass(const QString& commaSeparated) {
    QMutexLocker lock(&m_mutex);
    m_bypassEntries.clear();
    // 回环永远直连：HAR target 发现等内部 QNAM 请求打 127.0.0.1，被代理劫持会莫名失败。
    static const char* kImplied[] = { "localhost", "127.0.0.1", "::1" };
    QStringList items;
    for (const char* s : kImplied) items << QString::fromLatin1(s);
    items << commaSeparated.split(QLatin1Char(','), Qt::SkipEmptyParts);

    for (QString raw : items) {
        QString entry = raw.trimmed().toLower();
        if (entry.isEmpty()) continue;
        const int slash = entry.indexOf(QLatin1Char('/'));
        if (slash > 0) {
            // IPv4 CIDR（如 10.0.0.0/8）。
            QHostAddress base(entry.left(slash));
            bool prefixOk = false;
            const int prefix = entry.mid(slash + 1).toInt(&prefixOk);
            if (base.protocol() == QAbstractSocket::IPv4Protocol && prefixOk
                && prefix >= 0 && prefix <= 32) {
                m_bypassEntries.push_back(BypassEntry{entry, true, base, prefix});
            } else {
                qWarning("[WARN] --proxy-bypass entry '%s' is not a valid IPv4 CIDR, ignored",
                         entry.toUtf8().constData());
            }
            continue;
        }
        // 域名后缀 / 精确主机名 / IP 字面量（匹配逻辑见 bypassMatchLocked）。
        m_bypassEntries.push_back(BypassEntry{entry, false, QHostAddress(), 0});
    }
}

bool ProxyConfig::isBypassedForHost(const QString& host) const {
    if (host.isEmpty()) return false;
    QMutexLocker lock(&m_mutex);
    return bypassMatchLocked(host.toLower());
}

bool ProxyConfig::bypassMatchLocked(const QString& host) const {
    for (const BypassEntry& e : m_bypassEntries) {
        if (e.isCidr) {
            const QHostAddress addr(host);
            if (addr.protocol() != QAbstractSocket::IPv4Protocol
                || e.cidrBase.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const quint32 mask = e.cidrPrefix >= 32 ? 0xffffffffu
                                                    : (~0u << (32 - e.cidrPrefix));
            if ((addr.toIPv4Address() & mask) == (e.cidrBase.toIPv4Address() & mask)) {
                return true;
            }
            continue;
        }
        // 精确匹配：主机名 / IP 字面量 / localhost。
        if (host == e.pattern) return true;
        // 域名后缀：gwm.cn 命中 jira.gwm.cn（不带点的前缀不算，防 "xgwm.cn" 误匹配）。
        if (host.endsWith(QLatin1Char('.') + e.pattern)) return true;
    }
    return false;
}

void ProxyConfig::apply() {
    // GUI 线程调用。锁内取 pending 判 dirty，释锁后应用（不持锁阻塞 setProxy）。
    ProxySpec spec;
    bool dirty = false;
    {
        QMutexLocker lock(&m_mutex);
        dirty = m_dirty;
        if (dirty) spec = m_pending;
    }
    if (!dirty) return;

    // 记录生效值 + 变化时刻（两类路径共用；过渡期失败诊断依赖）。
    auto recordApplied = [this, &spec]() {
        QMutexLocker lock(&m_mutex);
        m_dirty = false;
        if (!(spec == m_applied)) {
            m_applied = spec;
            m_lastAppliedChangeMsec = QDateTime::currentMSecsSinceEpoch();
        }
    };

    // 配置了 bypass 列表：**不触碰任何全局 Qt 代理状态**（setApplicationProxy/
    // setApplicationProxyFactory 都不调），只更新生效值——渲染侧分流靠 main.cpp 注入的
    // Chromium flag（任何全局代理推送都会覆盖 flag，见 SeimiProxyFactory 注释），
    // /fetch 侧由其 QNAM 挂 fetchProxyFactory() 读生效值分流。
    // 未配置 bypass 保持历史全局单值路径，零行为变更。
    if (!m_bypassEntries.empty()) {
        recordApplied();
        return;
    }

    QNetworkProxy::setApplicationProxy(toQNetworkProxy(spec));
    recordApplied();
}

} // namespace seimi
