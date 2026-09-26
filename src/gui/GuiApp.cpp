// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "GuiApp.h"

#include "AppIcon.h"
#include "FaIconProvider.h"

#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>

#include <cstdio>

namespace seimi {

GuiApp::GuiApp(const Params& p)
    : m_cfg(p.cfg)
    , m_prefs(&m_cfg)
    , m_theme(&m_prefs)
    , m_service(ServiceInfo::Params{
          p.host, p.httpPort, p.wsPort, p.mcpPort, p.dataDir, p.password,
          p.windowed, p.uiDemo, p.uiDemoPage}, nullptr)
    , m_dash(DashboardModel::Deps{p.queue, p.proxy, p.cookies, p.mcp, p.env, &m_prefs})
    , m_thr(p.queue)
    , m_settings(&m_cfg, p.proxy, p.effectiveSettings, p.cliFlags)
    , m_cookiesModel(p.cookies)
    , m_logModel()
    , m_update()
    , m_tray(&m_prefs)
{
    m_update.init(p.dataDir);
    m_update.setManifestUrl(m_cfg.gui.manifestUrl);
    // manifestUrl 的持久化由 GuiPrefs 承担：UpdateManager 改动时同步回写。
    QObject::connect(&m_update, &UpdateManager::manifestUrlChanged, &m_prefs, [this]() {
        m_prefs.setManifestUrl(m_update.manifestUrl());
    });
}

GuiApp::~GuiApp() {
    delete m_engine; // 先于各单例析构（engine 的 QML 上下文引用它们）
}

void GuiApp::start() {
    // Basic 是可定制控件基底（视觉由全局 palette + 自绘组件承担）；
    // 必须在引擎创建前选定（QQC2 按引擎加载样式）。
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    m_engine = new QQmlApplicationEngine();
    // 导航图标（FaIcon）经 image://fa/ 提供：QSvgRenderer 光栅化 + 着色；
    // 应用品牌图标（admin-ui/favicon.svg 原色）经 image://appicon/ 提供。
    // 都必须在 QML 加载前注册。
    m_engine->addImageProvider(QStringLiteral("fa"), new FaIconProvider());
    m_engine->addImageProvider(QStringLiteral("appicon"),
                               new FaIconProvider(QStringLiteral(":/admin-ui/favicon.svg")));

    const char* kUri = "SeimiRender.Gui";
    qmlRegisterSingletonInstance(kUri, 1, 0, "GuiPrefs", &m_prefs);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Theme", &m_theme);
    qmlRegisterSingletonInstance(kUri, 1, 0, "ServiceInfo", &m_service);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Dashboard", &m_dash);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Throughput", &m_thr);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Settings", &m_settings);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Cookies", &m_cookiesModel);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Logs", &m_logModel);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Updater", &m_update);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Tray", &m_tray);

    // 更新交接完成 → 退出主进程（走 aboutToQuit 优雅关机链）。
    QObject::connect(&m_update, &UpdateManager::requestQuit,
                     qApp, &QCoreApplication::quit);
    // 托盘动作（GuiApp 非 QObject，用无上下文 lambda；连接寿命随 sender/GuiApp 一致）。
    QObject::connect(&m_tray, &TrayController::showRequested, [this]() { raiseWindow(); });
    QObject::connect(&m_tray, &TrayController::openConsoleRequested,
                     &m_service, &ServiceInfo::openAdminConsole);
    QObject::connect(&m_tray, &TrayController::checkUpdateRequested,
                     &m_update, &UpdateManager::checkNow);
    QObject::connect(&m_tray, &TrayController::quitRequested,
                     qApp, &QCoreApplication::quit);

    m_engine->load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (m_engine->rootObjects().isEmpty()) {
        std::fprintf(stderr, "FATAL: GUI Main.qml 加载失败（QML 模块缺失？）\n");
    } else if (QQuickWindow* w = qobject_cast<QQuickWindow*>(m_engine->rootObjects().value(0))) {
        // 窗口图标 = favicon.svg 多尺寸光栅化（AppIcon.h），资源管理器/任务栏同源。
        w->setIcon(appIcon());
    }

    // 自动检查更新（配置开启且配了 manifestUrl 才生效）。
    m_update.maybeAutoCheck(m_cfg.gui.autoCheckUpdates);
}

void GuiApp::raiseWindow() {
    if (!m_engine) return;
    if (QQuickWindow* w = qobject_cast<QQuickWindow*>(m_engine->rootObjects().value(0))) {
        w->showNormal();
        w->raise();
        w->requestActivate();
    }
}

} // namespace seimi
