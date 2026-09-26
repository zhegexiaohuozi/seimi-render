// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "ServiceInfo.h"

#include "../HttpServer.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QUrl>

namespace seimi {

ServiceInfo::ServiceInfo(const Params& p, QObject* parent)
    : QObject(parent)
    , m_host(p.host)
    , m_httpPort(p.httpPort)
    , m_wsPort(p.wsPort)
    , m_mcpPort(p.mcpPort)
    , m_dataDir(p.dataDir)
    , m_passwordEnabled(!p.password.isEmpty())
    , m_windowed(p.windowed)
    , m_uiDemo(p.uiDemo)
    , m_uiDemoPage(p.uiDemoPage)
    , m_password(p.password)
{
}

QString ServiceInfo::appIconUrl() const {
    // 品牌图标 = admin-ui/favicon.svg（qrc，经 GuiApp 注册的 appicon provider 原色光栅化）。
    // QML 侧拼接 /<devicePixelRatio> 请求对应 DPR 的位图。
    return QStringLiteral("image://appicon");
}

QString ServiceInfo::adminUrl() const {
    const QString host = (m_host == QLatin1String("0.0.0.0")) ? QStringLiteral("127.0.0.1")
                                                              : m_host;
    return QStringLiteral("http://%1:%2/").arg(host).arg(m_httpPort);
}

bool ServiceInfo::exposed() const {
    return m_host != QLatin1String("127.0.0.1") && m_host != QLatin1String("localhost");
}

bool ServiceInfo::trayAvailable() const {
    return QSystemTrayIcon::isSystemTrayAvailable();
}

QString ServiceInfo::platformKey() const {
    QString product = QSysInfo::productType();
    if (product == QLatin1String("osx")) product = QStringLiteral("macos");
    QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("x86_64")) arch = QStringLiteral("x64");
    return product + QLatin1Char('-') + arch;
}

void ServiceInfo::openDataDir() const {
    QDir().mkpath(m_dataDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_dataDir));
}

void ServiceInfo::openAdminConsole() const {
    QDesktopServices::openUrl(QUrl(adminUrl()));
}

void ServiceInfo::copyText(const QString& text) const {
    if (QClipboard* cb = QGuiApplication::clipboard()) cb->setText(text);
}

QString ServiceInfo::accessToken() const {
    // 未启用密码时返回空——QML 据此显示「未启用」。本地操作台与 --password 持有者
    // 同权，展示 token 不构成额外暴露。
    return m_passwordEnabled
        ? QString::fromStdString(HttpServer::computeToken(m_password.toStdString()))
        : QString();
}

} // namespace seimi
