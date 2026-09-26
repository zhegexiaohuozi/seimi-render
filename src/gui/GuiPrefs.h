// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../AppConfig.h"

#include <QObject>
#include <QString>

namespace seimi {

// GUI 偏好（config.json 的 gui.* 段）：主题/语言/侧栏折叠/动效/更新偏好。
// QML 单例（qmlRegisterSingletonInstance）。全部 GUI 线程。
//
// effectiveDark：theme=="dark"，或 theme=="system" 且系统深色。
// 系统深色探测用轮询（Qt 6.7 的 QStyleHints::colorScheme 变更信号不全），
// 由 DashboardModel 的 2s 刷新顺带调 syncSystemDark()。
class GuiPrefs : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString locale READ locale WRITE setLocale NOTIFY localeChanged)
    Q_PROPERTY(bool collapseNav READ collapseNav WRITE setCollapseNav NOTIFY collapseNavChanged)
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(QString updateChannel READ updateChannel WRITE setUpdateChannel NOTIFY updateChannelChanged)
    Q_PROPERTY(bool autoCheckUpdates READ autoCheckUpdates WRITE setAutoCheckUpdates NOTIFY autoCheckUpdatesChanged)
    Q_PROPERTY(QString manifestUrl READ manifestUrl WRITE setManifestUrl NOTIFY manifestUrlChanged)
    Q_PROPERTY(bool effectiveDark READ effectiveDark NOTIFY effectiveDarkChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY effectiveDarkChanged)
public:
    // cfg 由 GuiApp 持有的 AppConfig（写入即落盘）。parent 必须 GUI 线程。
    explicit GuiPrefs(AppConfig* cfg, QObject* parent = nullptr);

    QString theme() const { return m_cfg->gui.theme; }
    void setTheme(const QString& t);
    QString locale() const { return m_cfg->gui.locale; }
    void setLocale(const QString& l);
    bool collapseNav() const { return m_cfg->gui.collapseNav; }
    void setCollapseNav(bool v);
    bool reducedMotion() const { return m_cfg->gui.reducedMotion; }
    void setReducedMotion(bool v);
    QString updateChannel() const { return m_cfg->gui.updateChannel; }
    void setUpdateChannel(const QString& c);
    bool autoCheckUpdates() const { return m_cfg->gui.autoCheckUpdates; }
    void setAutoCheckUpdates(bool v);
    QString manifestUrl() const { return m_cfg->gui.manifestUrl; }
    void setManifestUrl(const QString& u);

    bool effectiveDark() const { return m_effectiveDark; }
    bool systemDark() const { return m_systemDark; }

    // 周期调（2s）：跟随系统深浅变化。返回 effectiveDark 是否变化。
    bool syncSystemDark();

signals:
    void themeChanged();
    void localeChanged();
    void collapseNavChanged();
    void reducedMotionChanged();
    void updateChannelChanged();
    void autoCheckUpdatesChanged();
    void manifestUrlChanged();
    void effectiveDarkChanged();

private:
    void recomputeEffective();

    AppConfig* m_cfg;      // GuiApp 持有，非所有权
    bool m_systemDark = false;
    bool m_effectiveDark = false;
};

} // namespace seimi
