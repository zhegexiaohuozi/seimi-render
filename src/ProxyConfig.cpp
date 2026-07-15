#include "ProxyConfig.h"

#include <QNetworkProxy>
#include <QString>

namespace seimi {

void ProxyConfig::apply() {
    // GUI 线程调用。锁内取 pending 判 dirty，释锁后调 setApplicationProxy（不持锁阻塞 setProxy）。
    ProxySpec spec;
    bool dirty;
    {
        QMutexLocker lock(&m_mutex);
        dirty = m_dirty;
        if (dirty) spec = m_pending;
    }
    if (!dirty) return;

    // Direct 显式声明 NoProxy（与"不设置"等价且明确），不强制覆盖 Qt 默认代理工厂行为。
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
            proxy.setType(QNetworkProxy::NoProxy);
            break;
    }
    QNetworkProxy::setApplicationProxy(proxy);

    {
        QMutexLocker lock(&m_mutex);
        m_dirty = false;
    }
}

} // namespace seimi
