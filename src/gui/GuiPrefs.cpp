// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "GuiPrefs.h"

#include <QGuiApplication>
#include <QStyleHints>

namespace seimi {

GuiPrefs::GuiPrefs(AppConfig* cfg, QObject* parent)
    : QObject(parent)
    , m_cfg(cfg)
{
    syncSystemDark();
    recomputeEffective();
}

void GuiPrefs::setTheme(const QString& t) {
    if (t == QLatin1String("light") || t == QLatin1String("dark")
        || t == QLatin1String("system")) {
        if (m_cfg->gui.theme == t) return;
        m_cfg->gui.theme = t;
        m_cfg->save();
        emit themeChanged();
        recomputeEffective();
    }
}

void GuiPrefs::setLocale(const QString& l) {
    const QString v = (l == QLatin1String("en")) ? QStringLiteral("en")
                                                 : QStringLiteral("zh");
    if (m_cfg->gui.locale == v) return;
    m_cfg->gui.locale = v;
    m_cfg->save();
    emit localeChanged();
}

void GuiPrefs::setCollapseNav(bool v) {
    if (m_cfg->gui.collapseNav == v) return;
    m_cfg->gui.collapseNav = v;
    m_cfg->save();
    emit collapseNavChanged();
}

void GuiPrefs::setReducedMotion(bool v) {
    if (m_cfg->gui.reducedMotion == v) return;
    m_cfg->gui.reducedMotion = v;
    m_cfg->save();
    emit reducedMotionChanged();
}

void GuiPrefs::setUpdateChannel(const QString& c) {
    const QString v = (c == QLatin1String("beta")) ? QStringLiteral("beta")
                                                   : QStringLiteral("stable");
    if (m_cfg->gui.updateChannel == v) return;
    m_cfg->gui.updateChannel = v;
    m_cfg->save();
    emit updateChannelChanged();
}

void GuiPrefs::setAutoCheckUpdates(bool v) {
    if (m_cfg->gui.autoCheckUpdates == v) return;
    m_cfg->gui.autoCheckUpdates = v;
    m_cfg->save();
    emit autoCheckUpdatesChanged();
}

void GuiPrefs::setManifestUrl(const QString& u) {
    const QString v = u.trimmed();
    if (m_cfg->gui.manifestUrl == v) return;
    m_cfg->gui.manifestUrl = v;
    m_cfg->save();
    emit manifestUrlChanged();
}

bool GuiPrefs::syncSystemDark() {
    bool dark = false;
    if (const QStyleHints* hints = QGuiApplication::styleHints()) {
        dark = (hints->colorScheme() == Qt::ColorScheme::Dark);
    }
    if (dark == m_systemDark) return false;
    m_systemDark = dark;
    recomputeEffective();
    return true;
}

void GuiPrefs::recomputeEffective() {
    const bool dark = m_cfg->gui.theme == QLatin1String("dark")
                   || (m_cfg->gui.theme == QLatin1String("system") && m_systemDark);
    if (dark == m_effectiveDark) return;
    m_effectiveDark = dark;
    emit effectiveDarkChanged();
}

} // namespace seimi
