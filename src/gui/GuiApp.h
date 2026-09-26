// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CookiesModel.h"
#include "DashboardModel.h"
#include "GuiPrefs.h"
#include "LogModel.h"
#include "ServiceInfo.h"
#include "SettingsModel.h"
#include "Theme.h"
#include "TrayController.h"
#include "../AppConfig.h"
#include "../CookieStore.h"
#include "../Environment.h"
#include "../McpServer.h"
#include "../ProxyConfig.h"
#include "../RenderQueue.h"
#include "../UpdateManager.h"

#include <QVariantMap>

#include <memory>

class QQmlApplicationEngine;
class QQuickWindow;

namespace seimi {

// GUI 装配器（--gui 模式专用）：创建 QML 引擎、注册全部单例、建托盘、接线。
// 生命周期：main 以 unique_ptr 持有，存活到 app.exec() 返回之后（aboutToQuit 的
// 关机链跑完，GuiApp 析构时先销毁 engine 再析构各单例——成员声明顺序保证这一点）。
class GuiApp {
public:
    struct Params {
        AppConfig cfg;                       // 合并 CLI 后的配置副本（GuiApp 独占持有）
        QVariantMap effectiveSettings;       // 表单初始化用（生效值快照）
        QVariantMap cliFlags;                // CLI 显式覆盖标记
        QString dataDir;
        RenderQueue* queue = nullptr;
        ProxyConfig* proxy = nullptr;
        CookieStore* cookies = nullptr;
        McpServer* mcp = nullptr;
        Environment* env = nullptr;
        QString host;
        int httpPort = 8088;
        int wsPort = 8089;
        int mcpPort = 8090;
        QString password;
        bool windowed = false;
        bool uiDemo = false;                  // --ui-demo：导航追加控件陈列室页（验收用）
        QString uiDemoPage;                   // --ui-demo=<page>：启动直达页
    };

    explicit GuiApp(const Params& p);
    ~GuiApp();

    void start();                 // 装配 QML 并显示主窗口（pool.start() 前调用）
    void raiseWindow();           // 托盘「显示」用

    UpdateManager* updateManager() { return &m_update; }

private:
    // 声明顺序 = 析构顺序的逆序：engine 最后声明 → 最先析构（它引用前面的单例）。
    AppConfig m_cfg;
    GuiPrefs m_prefs;
    Theme m_theme;
    ServiceInfo m_service;
    DashboardModel m_dash;
    ThroughputModel m_thr;
    SettingsModel m_settings;
    CookiesModel m_cookiesModel;
    LogModel m_logModel;
    UpdateManager m_update;
    TrayController m_tray;
    QQmlApplicationEngine* m_engine = nullptr; // start() 中创建，析构函数 delete
};

} // namespace seimi
