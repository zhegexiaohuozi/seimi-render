// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "TrayController.h"
#include "AppIcon.h"
#include "GuiPrefs.h"

#include <QApplication>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>

namespace seimi {

TrayController::TrayController(GuiPrefs* prefs, QObject* parent)
    : QObject(parent)
    , m_prefs(prefs)
{
    m_tray = new QSystemTrayIcon(this);
    const QIcon icon = appIcon();
    m_tray->setIcon(icon.isNull() ? QApplication::style()->standardIcon(QStyle::SP_ComputerIcon)
                                  : icon);
    m_tray->setToolTip(QStringLiteral("seimi-render"));

    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    emit showRequested();
                }
            });

    rebuildMenu();
    connect(prefs, &GuiPrefs::localeChanged, this, [this]() { rebuildMenu(); });

    m_tray->show();
}

bool TrayController::available() const {
    return QSystemTrayIcon::isSystemTrayAvailable();
}

QString TrayController::trPair(const char* zh, const char* en, bool zhLocale) {
    return zhLocale ? QString::fromUtf8(zh) : QString::fromLatin1(en);
}

void TrayController::rebuildMenu() {
    const bool zh = !m_prefs || m_prefs->locale() == QLatin1String("zh");
    delete m_menu;
    m_menu = new QMenu();

    QAction* show = m_menu->addAction(trPair("显示主窗口", "Show main window", zh));
    connect(show, &QAction::triggered, this, &TrayController::showRequested);

    QAction* web = m_menu->addAction(trPair("打开 Web 控制台", "Open web console", zh));
    connect(web, &QAction::triggered, this, &TrayController::openConsoleRequested);

    QAction* upd = m_menu->addAction(trPair("检查更新", "Check for updates", zh));
    connect(upd, &QAction::triggered, this, &TrayController::checkUpdateRequested);

    m_menu->addSeparator();
    QAction* quit = m_menu->addAction(trPair("退出", "Quit", zh));
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);

    m_tray->setContextMenu(m_menu);
}

void TrayController::onHiddenToTray() {
    if (m_balloonShown || !m_tray) return;
    m_balloonShown = true;
    const bool zh = !m_prefs || m_prefs->locale() == QLatin1String("zh");
    m_tray->showMessage(
        QStringLiteral("seimi-render"),
        trPair("已最小化到托盘，右键托盘图标可退出。", "Minimized to tray. Right-click the tray icon to quit.", zh),
        QSystemTrayIcon::Information, 4000);
}

} // namespace seimi
