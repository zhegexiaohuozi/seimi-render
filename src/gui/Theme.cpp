// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "Theme.h"
#include "GuiPrefs.h"

#include <QFontDatabase>

namespace seimi {

// 色值来源：用户提供的 Clash Verge Rev 深浅截图取色（IMPLEMENTATION_BRIEF.md §2.1，
// 截图优先于上游 dev 分支默认值）。divider/hover 为低透明度黑/白分层色。
const QColor Theme::lightCanvas        = QColor(0xEC, 0xEC, 0xEC);
const QColor Theme::lightSidebar       = QColor(0xF5, 0xF5, 0xF5);
const QColor Theme::lightCard          = QColor(0xFF, 0xFF, 0xFF);
const QColor Theme::lightInset         = QColor(0xF5, 0xF5, 0xF5);
const QColor Theme::lightText          = QColor(0x00, 0x00, 0x00);
const QColor Theme::lightSecondary     = QColor(0x8A, 0x8A, 0x8E);
const QColor Theme::lightNavSelected   = QColor(0xD8, 0xE2, 0xF5);
const QColor Theme::lightDivider       = QColor(0x00, 0x00, 0x00, 18);   // 黑 7%
const QColor Theme::lightHover         = QColor(0x00, 0x00, 0x00, 10);   // 黑 4%
const QColor Theme::lightPrimary       = QColor(0x34, 0x78, 0xF6);
const QColor Theme::lightPrimaryButton = QColor(0x34, 0x78, 0xF6);
const QColor Theme::lightSuccess       = QColor(0x32, 0xA8, 0x52);
const QColor Theme::lightWarning       = QColor(0xFF, 0xAA, 0x32);
const QColor Theme::lightDanger        = QColor(0xE3, 0x4B, 0x42);

const QColor Theme::darkCanvas         = QColor(0x1E, 0x1F, 0x26);
const QColor Theme::darkSidebar        = QColor(0x2E, 0x30, 0x3C);
const QColor Theme::darkCard           = QColor(0x28, 0x2A, 0x35);
const QColor Theme::darkInset          = QColor(0x3E, 0x40, 0x4B);
const QColor Theme::darkText           = QColor(0xFF, 0xFF, 0xFF);
const QColor Theme::darkSecondary      = QColor(0x9D, 0x9E, 0xA8);
const QColor Theme::darkNavSelected    = QColor(0x32, 0x4D, 0x7D);
const QColor Theme::darkDivider        = QColor(0xFF, 0xFF, 0xFF, 23);   // 白 9%
const QColor Theme::darkHover          = QColor(0xFF, 0xFF, 0xFF, 13);   // 白 5%
const QColor Theme::darkPrimary        = QColor(0x3B, 0x82, 0xF7);
const QColor Theme::darkPrimaryButton  = QColor(0x49, 0x8B, 0xF8);
const QColor Theme::darkSuccess        = QColor(0x62, 0xCD, 0x63);
const QColor Theme::darkWarning        = QColor(0xFF, 0xAA, 0x32);
const QColor Theme::darkDanger         = QColor(0xFF, 0x60, 0x58);

Theme::Theme(GuiPrefs* prefs, QObject* parent)
    : QObject(parent)
{
    m_dark = prefs->effectiveDark();
    connect(prefs, &GuiPrefs::effectiveDarkChanged, this, [this, prefs]() {
        const bool dark = prefs->effectiveDark();
        if (dark == m_dark) return;
        m_dark = dark;
        emit colorsChanged();
    });
    m_reduceMotion = prefs->reducedMotion();
    connect(prefs, &GuiPrefs::reducedMotionChanged, this, [this, prefs]() {
        const bool rm = prefs->reducedMotion();
        if (rm == m_reduceMotion) return;
        m_reduceMotion = rm;
        emit reduceMotionChanged();
    });

    // 字体按 QFontDatabase 实测解析（不用 CSS 逗号字体栈直塞 QML）。
    m_fontFamily = resolveFontFamily({
        QStringLiteral("Microsoft YaHei UI"),   // Windows 首选
        QStringLiteral("Microsoft YaHei"),
        QStringLiteral("PingFang SC"),          // macOS
        QStringLiteral("Hiragino Sans GB"),
        QStringLiteral("Noto Sans CJK SC"),     // Linux 常见
        QStringLiteral("Source Han Sans SC"),
        QStringLiteral("WenQuanYi Micro Hei"),
    });
    m_monoFamily = resolveFontFamily({
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Consolas"),
        QStringLiteral("SF Mono"),
        QStringLiteral("Menlo"),
        QStringLiteral("DejaVu Sans Mono"),
    });
}

QString Theme::resolveFontFamily(const QStringList& candidates) {
    const QFontDatabase db;
    const QStringList families = db.families();
    for (const QString& name : candidates) {
        if (families.contains(name, Qt::CaseInsensitive)) return name;
    }
    return QString(); // 回退系统默认字体
}

} // namespace seimi
