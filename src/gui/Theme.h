// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QObject>
#include <QString>
#include <QColor>

namespace seimi {

class GuiPrefs;

// 主题令牌（QML 单例）。配色与尺寸取自用户提供的 Clash Verge Rev 深浅截图
// （见 IMPLEMENTATION_BRIEF.md 固定视觉规格，截图优先）：
//   浅色 canvas #ECECEC / sidebar #F5F5F5 / card #FFFFFF / inset #F5F5F5；
//   深色 canvas #1E1F26 / sidebar #2E303C / card #282A35 / inset #3E404B。
// 主要靠分层背景与留白区分模块；卡片不加描边阴影（分隔线用低透明度黑/白）。
// 控件基底为 Qt Quick Controls Basic；全局 palette 在 Main.qml 根部绑定本类。
class Theme : public QObject {
    Q_OBJECT
    // —— 分层背景与文本 ——
    Q_PROPERTY(QColor canvas READ canvas NOTIFY colorsChanged)
    Q_PROPERTY(QColor sidebar READ sidebar NOTIFY colorsChanged)
    Q_PROPERTY(QColor card READ card NOTIFY colorsChanged)
    Q_PROPERTY(QColor inset READ inset NOTIFY colorsChanged)
    Q_PROPERTY(QColor text READ text NOTIFY colorsChanged)
    Q_PROPERTY(QColor secondaryText READ secondaryText NOTIFY colorsChanged)
    Q_PROPERTY(QColor navSelected READ navSelected NOTIFY colorsChanged)
    Q_PROPERTY(QColor divider READ divider NOTIFY colorsChanged)
    Q_PROPERTY(QColor hover READ hover NOTIFY colorsChanged)
    // —— 品牌与状态 ——
    Q_PROPERTY(QColor primary READ primary NOTIFY colorsChanged)
    Q_PROPERTY(QColor primaryButton READ primaryButton NOTIFY colorsChanged)
    Q_PROPERTY(QColor success READ success NOTIFY colorsChanged)
    Q_PROPERTY(QColor warning READ warning NOTIFY colorsChanged)
    Q_PROPERTY(QColor danger READ danger NOTIFY colorsChanged)
    // —— 密度令牌（逻辑 px，CONSTANT）——
    Q_PROPERTY(int sidebarWidth READ sidebarWidth CONSTANT)
    Q_PROPERTY(int headerHeight READ headerHeight CONSTANT)
    Q_PROPERTY(int pagePadding READ pagePadding CONSTANT)
    Q_PROPERTY(int gap READ gap CONSTANT)
    Q_PROPERTY(int cardPadding READ cardPadding CONSTANT)
    Q_PROPERTY(int cardRadius READ cardRadius CONSTANT)
    Q_PROPERTY(int cardHeaderHeight READ cardHeaderHeight CONSTANT)
    Q_PROPERTY(int navHeight READ navHeight CONSTANT)
    Q_PROPERTY(int iconBoxSize READ iconBoxSize CONSTANT)
    Q_PROPERTY(int motionMs READ motionMs CONSTANT)
    // —— 字号（逻辑 px）——
    Q_PROPERTY(int titleSize READ titleSize CONSTANT)
    Q_PROPERTY(int cardTitleSize READ cardTitleSize CONSTANT)
    Q_PROPERTY(int bodySize READ bodySize CONSTANT)
    Q_PROPERTY(int captionSize READ captionSize CONSTANT)
    // —— 字体（构造期按 QFontDatabase 实测解析，避免 CSS 字体栈直塞）——
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    Q_PROPERTY(QString monoFamily READ monoFamily CONSTANT)
    // —— 动效开关（跟 GuiPrefs.reducedMotion）——
    Q_PROPERTY(bool reduceMotion READ reduceMotion NOTIFY reduceMotionChanged)
public:
    explicit Theme(GuiPrefs* prefs, QObject* parent = nullptr);

    QColor canvas() const { return m_dark ? darkCanvas : lightCanvas; }
    QColor sidebar() const { return m_dark ? darkSidebar : lightSidebar; }
    QColor card() const { return m_dark ? darkCard : lightCard; }
    QColor inset() const { return m_dark ? darkInset : lightInset; }
    QColor text() const { return m_dark ? darkText : lightText; }
    QColor secondaryText() const { return m_dark ? darkSecondary : lightSecondary; }
    QColor navSelected() const { return m_dark ? darkNavSelected : lightNavSelected; }
    QColor divider() const { return m_dark ? darkDivider : lightDivider; }
    QColor hover() const { return m_dark ? darkHover : lightHover; }
    QColor primary() const { return m_dark ? darkPrimary : lightPrimary; }
    QColor primaryButton() const { return m_dark ? darkPrimaryButton : lightPrimaryButton; }
    QColor success() const { return m_dark ? darkSuccess : lightSuccess; }
    QColor warning() const { return m_dark ? darkWarning : lightWarning; }
    QColor danger() const { return m_dark ? darkDanger : lightDanger; }

    int sidebarWidth() const { return 200; }
    int headerHeight() const { return 58; }
    int pagePadding() const { return 10; }
    int gap() const { return 12; }
    int cardPadding() const { return 16; }
    int cardRadius() const { return 8; }
    int cardHeaderHeight() const { return 54; }
    int navHeight() const { return 48; }
    int iconBoxSize() const { return 38; }
    int motionMs() const { return 120; }

    int titleSize() const { return 20; }
    int cardTitleSize() const { return 18; }
    int bodySize() const { return 14; }
    int captionSize() const { return 12; }

    QString fontFamily() const { return m_fontFamily; }
    QString monoFamily() const { return m_monoFamily; }
    bool reduceMotion() const { return m_reduceMotion; }

    // 透明色助手（QML 侧 Qt.rgba 的便捷版；#RRGGBBAA 搬运事故的防线）。
    Q_INVOKABLE QColor alpha(const QColor& c, double a) const {
        return QColor(c.red(), c.green(), c.blue(), int(a * 255));
    }

signals:
    void colorsChanged();
    void reduceMotionChanged();

private:
    void onDarkChanged();
    static QString resolveFontFamily(const QStringList& candidates);

    bool m_dark = false;
    bool m_reduceMotion = false;
    QString m_fontFamily;
    QString m_monoFamily;

    // —— 浅色（截图取色）——
    static const QColor lightCanvas, lightSidebar, lightCard, lightInset;
    static const QColor lightText, lightSecondary, lightNavSelected;
    static const QColor lightDivider, lightHover;
    static const QColor lightPrimary, lightPrimaryButton;
    static const QColor lightSuccess, lightWarning, lightDanger;
    // —— 深色 ——
    static const QColor darkCanvas, darkSidebar, darkCard, darkInset;
    static const QColor darkText, darkSecondary, darkNavSelected;
    static const QColor darkDivider, darkHover;
    static const QColor darkPrimary, darkPrimaryButton;
    static const QColor darkSuccess, darkWarning, darkDanger;
};

} // namespace seimi
