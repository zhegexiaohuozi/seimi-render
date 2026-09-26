// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QObject>
#include <QString>

class QQuickWindow;

namespace seimi {

// 服务元信息（QML 单例）：地址/端口/数据目录/版本/托盘可用性 + 少量桌面动作
//（打开目录、打开 Web 控制台、复制到剪贴板）。全部只读 + Q_INVOKABLE，GUI 线程。
class ServiceInfo : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString host READ host CONSTANT)
    Q_PROPERTY(QString httpAddr READ httpAddr CONSTANT)      // host:port（展示用）
    Q_PROPERTY(int httpPort READ httpPort CONSTANT)
    Q_PROPERTY(int wsPort READ wsPort CONSTANT)
    Q_PROPERTY(int mcpPort READ mcpPort CONSTANT)
    Q_PROPERTY(QString adminUrl READ adminUrl CONSTANT)      // http://127.0.0.1:<port>/
    Q_PROPERTY(QString dataDir READ dataDir CONSTANT)
    Q_PROPERTY(bool passwordEnabled READ passwordEnabled CONSTANT)
    Q_PROPERTY(bool exposed READ exposed CONSTANT)           // 非 127.0.0.1/localhost
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)
    Q_PROPERTY(bool windowed READ windowed CONSTANT)         // GUI 是否以原生平台运行
    Q_PROPERTY(QString platformKey READ platformKey CONSTANT)
    Q_PROPERTY(QString appIconUrl READ appIconUrl CONSTANT)  // 品牌图标 provider 基址（image://appicon）
    Q_PROPERTY(bool uiDemo READ uiDemo CONSTANT)             // --ui-demo：显示控件陈列室页
    Q_PROPERTY(QString uiDemoPage READ uiDemoPage CONSTANT)  // 启动直达页（验收截图用）
public:
    struct Params {
        QString host;
        int httpPort = 8088;
        int wsPort = 8089;
        int mcpPort = 8090;
        QString dataDir;
        QString password;        // 原始口令（仅派生 token 用，绝不暴露 QML；空 = 未启用）
        bool windowed = false;
        bool uiDemo = false;     // --ui-demo（验收：控件陈列室页）
        QString uiDemoPage;      // --ui-demo=<page>（gallery/settings/logs/…）
    };
    explicit ServiceInfo(const Params& p, QObject* parent = nullptr);

    QString host() const { return m_host; }
    QString httpAddr() const { return m_host + QLatin1Char(':') + QString::number(m_httpPort); }
    int httpPort() const { return m_httpPort; }
    int wsPort() const { return m_wsPort; }
    int mcpPort() const { return m_mcpPort; }
    QString adminUrl() const;
    QString dataDir() const { return m_dataDir; }
    bool passwordEnabled() const { return m_passwordEnabled; }
    bool exposed() const;
    bool trayAvailable() const;
    bool windowed() const { return m_windowed; }
    bool uiDemo() const { return m_uiDemo; }
    QString uiDemoPage() const { return m_uiDemoPage; }
    QString platformKey() const;
    QString appIconUrl() const;   // "image://appicon"（QML 拼接 /<dpr>；favicon.svg 原色）

    Q_INVOKABLE void openDataDir() const;      // 资源管理器/Finder 打开 data 目录
    Q_INVOKABLE void openAdminConsole() const; // 默认浏览器打开 admin-ui
    Q_INVOKABLE void copyText(const QString& text) const;
    // 登录态页配对展示用：启用密码时返回 token（本地操作台场景，等价 --password 持有者）。
    Q_INVOKABLE QString accessToken() const;

private:
    QString m_host;
    int m_httpPort, m_wsPort, m_mcpPort;
    QString m_dataDir;
    bool m_passwordEnabled;
    bool m_windowed;
    bool m_uiDemo = false;
    QString m_uiDemoPage;
    QString m_password; // 仅用于派生 token，不暴露 QML
};

} // namespace seimi
