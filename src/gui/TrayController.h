// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QMenu;
class QIcon;

namespace seimi {

class GuiPrefs;

// 系统托盘（C++ QSystemTrayIcon；QtWidgets 已链接，不引 Qt.labs.platform）。
// 菜单：显示主窗口 / 打开 Web 控制台 / 检查更新 / 退出。语言切换时重建菜单文案。
class TrayController : public QObject {
    Q_OBJECT
public:
    // 图标 = favicon.svg 多尺寸光栅化（AppIcon.h），失败回退标准图标。
    explicit TrayController(GuiPrefs* prefs, QObject* parent = nullptr);

    bool available() const;
    // 首次隐藏到托盘时的一次性气泡提示（QML onClosing 调）。
    Q_INVOKABLE void onHiddenToTray();

signals:
    void showRequested();
    void openConsoleRequested();
    void checkUpdateRequested();
    void quitRequested();

private:
    void rebuildMenu();
    static QString trPair(const char* zh, const char* en, bool zhLocale);

    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    GuiPrefs* m_prefs = nullptr;
    bool m_balloonShown = false;
};

} // namespace seimi
