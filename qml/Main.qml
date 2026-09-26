// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "components"
import "pages"

// 外壳（Clash Verge Rev 截图节奏）：
//   左：固定 200 侧栏 = 品牌区(admin-ui 原始 logo) / 导航 / 弹性空白 / 底部资源摘要。
//   右：固定 58 页头(页面标题 + 主题/语言 + 窗口三键) + 可滚动内容。
// 端点/状态/速率/资源占用在侧栏摘要（无独立底部状态条）。右侧滚动不带动页头与侧栏。
ApplicationWindow {
    id: window
    width: 1100; height: 850
    minimumWidth: 780; minimumHeight: 600
    visible: true
    title: "SeimiRender"
    color: Theme.canvas
    // 原生窗口装饰：系统标题栏（含 SeimiRender 与系统三键）/系统外框/投影/圆角。
    // 拖动、双击最大化、边缘缩放、贴靠布局全部交给系统；此前自绘无边框方案已移除
    //（FramelessWindowHint 在 Qt 6.7 的 QML Window 无 startSystemMove，拖拽必然失效）。

    // Basic 控件基底 + 全局 palette：深浅主题下无残留默认浅色控件。
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize
    palette.window: Theme.canvas
    palette.windowText: Theme.text
    palette.base: Theme.card
    palette.alternateBase: Theme.inset
    palette.text: Theme.text
    palette.placeholderText: Theme.secondaryText
    palette.button: Theme.card
    palette.buttonText: Theme.text
    palette.highlight: Theme.primary
    palette.highlightedText: "#FFFFFF"
    palette.link: Theme.primary
    palette.light: Theme.secondaryText
    palette.mid: Theme.secondaryText
    palette.dark: Theme.secondaryText

    readonly property bool frameless: false   // 原生窗口装饰（历史自绘标题栏方案的开关，已停用保留）
    // 稳定页面 id（永不重排，一切跳转/可见性/标题都引用它）：
    //   0=概览 1=设置 2=更新 3=日志 4=登录态 5=控件陈列室(--ui-demo 专属)。
    // 侧栏展示顺序由 navOrder 单独控制，与页面身份解耦——改菜单顺序只动 navOrder。
    readonly property var navOrder: ServiceInfo.uiDemo
        ? [0, 4, 3, 1, 2, 5]      // 概览 / 登录态 / 日志 / 设置 / 更新 / 陈列室
        : [0, 4, 3, 1, 2]         // 概览 / 登录态 / 日志 / 设置 / 更新
    // 初始页：--ui-demo=<page> 直达（验收截图用）；默认概览。
    property int pageIndex: ServiceInfo.uiDemo
        ? Math.max(0, ["dashboard","settings","update","logs","cookies","gallery"]
                   .indexOf(ServiceInfo.uiDemoPage))
        : 0
    // 页面可视高度（滚动区高度）——需要内部固定高度列表的页面（日志）绑定。
    property real pageViewportHeight: 600
    // --ui-demo：追加控件陈列室页（验收用，第 6 页）。
    readonly property int galleryIndex: ServiceInfo.uiDemo ? 5 : -1
    readonly property var pageNames: ServiceInfo.uiDemo
        ? [Strings.t("nav.dashboard"), Strings.t("nav.settings"), Strings.t("nav.update"),
           Strings.t("nav.logs"), Strings.t("nav.cookies"), Strings.t("gallery.title")]
        : [Strings.t("nav.dashboard"), Strings.t("nav.settings"), Strings.t("nav.update"),
           Strings.t("nav.logs"), Strings.t("nav.cookies")]
    // 导航图标 = Font Awesome Free 6.7.2 官方 SVG 名（FaIcon 按 qrc 加载）。
    readonly property var pageGlyphs: ServiceInfo.uiDemo
        ? ["house", "sliders", "arrows-rotate", "file-lines", "lock", "microchip"]
        : ["house", "sliders", "arrows-rotate", "file-lines", "lock"]

    onClosing: function(close) {
        if (ServiceInfo.trayAvailable) {
            close.accepted = false;
            window.hide();
            Tray.onHiddenToTray();
        } else {
            close.accepted = false;
            quitConfirm.open();
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ============ 侧栏 ============
        Rectangle {
            Layout.preferredWidth: Theme.sidebarWidth
            Layout.minimumWidth: Theme.sidebarWidth
            Layout.maximumWidth: Theme.sidebarWidth
            Layout.fillHeight: true
            color: Theme.sidebar

            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.divider }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // —— 品牌区：favicon.svg 原色（provider 按 DPR 光栅化；不加底座/描边）——
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.headerHeight
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20; anchors.rightMargin: 12
                        spacing: 10
                        Image {
                            Layout.preferredWidth: 28; Layout.preferredHeight: 28
                            sourceSize: Qt.size(28, 28)
                            fillMode: Image.PreserveAspectFit
                            source: ServiceInfo.appIconUrl + "/" + Screen.devicePixelRatio
                        }
                        UiText {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: "SeimiRender"
                            font.pixelSize: 19
                            font.weight: Font.Bold
                        }
                    }
                }

                // —— 导航 ——
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10; Layout.rightMargin: 10
                    Layout.topMargin: 6
                    spacing: 8
                    Repeater {
                        model: window.navOrder.length
                        NavItem {
                            required property int index
                            // 本菜单项对应的稳定页面 id（导航顺序与页面身份解耦）。
                            readonly property int pageId: window.navOrder[index]
                            Layout.fillWidth: true
                            text: window.pageNames[pageId]
                            glyph: window.pageGlyphs[pageId]
                            selected: window.pageIndex === pageId
                            onClicked: {
                                window.pageIndex = pageId;
                                if (pageId === 0) Dashboard.refresh();
                                if (pageId === 4) Cookies.refresh();
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // —— 底部资源摘要（原状态条迁入）——
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: 20
                    spacing: 8
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Rectangle { width: 8; height: 8; radius: 4; color: Theme.success }
                        UiText {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            elide: Text.ElideMiddle
                            text: Strings.t("footer.running") + " · " + ServiceInfo.httpAddr
                            color: Theme.secondaryText
                            font.pixelSize: 11
                        }
                    }
                    MiniChart {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 48
                        samples: Throughput.completedSeries
                        emptyText: Strings.t("dashboard.no-samples")
                    }
                    UiText {
                        Layout.fillWidth: true
                        text: Strings.t("sidebar.submitted") + " " + Throughput.submittedRateText
                              + Strings.t("sidebar.per-sec") + "    "
                              + Strings.t("sidebar.completed") + " " + Throughput.completedRateText
                              + Strings.t("sidebar.per-sec")
                        color: Theme.secondaryText
                        font.pixelSize: 12
                    }
                    UiText {
                        Layout.fillWidth: true
                        text: Strings.t("footer.cpu") + " " + Dashboard.cpuText
                              + "    " + Strings.t("dashboard.env-rss") + " " + Dashboard.rssText
                        color: Theme.secondaryText
                        font.pixelSize: 12
                    }
                }
            }
        }

        // ============ 内容区 ============
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            spacing: 0

            // —— 页头（58）：页面标题 + 主题/语言切换 ——
            // 窗口操作（最小化/最大化/关闭、拖动、贴靠）由系统标题栏承担。
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.headerHeight
                color: Theme.sidebar

                Rectangle { anchors.bottom: parent.bottom; height: 1; width: parent.width; color: Theme.divider }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    spacing: 8

                    UiText {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: window.pageNames[window.pageIndex]
                        font.pixelSize: Theme.titleSize
                        font.weight: Font.Bold
                    }

                    // 主题切换
                    Rectangle {
                        visible: true
                        Layout.preferredWidth: 34; Layout.preferredHeight: 30
                        radius: 6
                        color: themeHover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: themeHover }
                        TapHandler {
                            onTapped: GuiPrefs.theme = GuiPrefs.effectiveDark ? "light" : "dark"
                        }
                        UiIcon {
                            anchors.centerIn: parent
                            glyph: GuiPrefs.effectiveDark ? "sun" : "moon"
                            width: 16; height: 16
                            ink: Theme.secondaryText
                        }
                    }
                    // 语言切换
                    Rectangle {
                        Layout.preferredWidth: 34; Layout.preferredHeight: 30
                        radius: 6
                        color: langHover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: langHover }
                        TapHandler {
                            onTapped: GuiPrefs.locale = (GuiPrefs.locale === "zh") ? "en" : "zh"
                        }
                        UiText {
                            anchors.centerIn: parent
                            text: GuiPrefs.locale === "zh" ? "EN" : "中"
                            color: Theme.secondaryText
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }

            // —— 可滚动内容：只滚内容，页头/侧栏固定 ——
            ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                onHeightChanged: window.pageViewportHeight = scroll.height

                ColumnLayout {
                    id: pageHost
                    x: Theme.pagePadding; y: Theme.pagePadding
                    width: Math.max(0, scroll.availableWidth - Theme.pagePadding * 2)
                    spacing: Theme.gap

                    DashboardPage {
                        Layout.fillWidth: true
                        visible: window.pageIndex === 0
                    }
                    SettingsPage {
                        Layout.fillWidth: true
                        visible: window.pageIndex === 1
                    }
                    UpdatePage {
                        Layout.fillWidth: true
                        visible: window.pageIndex === 2
                    }
                    LogsPage {
                        Layout.fillWidth: true
                        visible: window.pageIndex === 3
                    }
                    CookiesPage {
                        Layout.fillWidth: true
                        visible: window.pageIndex === 4
                    }
                    ControlsGallery {
                        Layout.fillWidth: true
                        visible: window.pageIndex === window.galleryIndex
                    }

                    // 底部安全留白（真实内容项，非遮挡/非虚增 contentHeight）：
                    // 内容高度 = pageHost 全部卡片 + 本项 16px。滚到底时最后一排卡片
                    // 底边与可见区底部恒有 16 逻辑像素背景空隙（圆角完整可见）；
                    // 内容不足一屏时同样保证 ≥16。展开/收起与数据变化经 ColumnLayout
                    // 的 implicitHeight 绑定链自动跟随，无需任何手工重算。
                    Item { Layout.fillWidth: true; implicitHeight: 16 }
                }
            }
        }
    }

    // 退出确认（运行中有任务时文案带数量）。
    ConfirmPopup {
        id: quitConfirm
        danger: true
        title: Strings.t("quit.title")
        message: Dashboard.queueRunning > 0
            ? Strings.t("quit.running").arg(Dashboard.queueRunning)
            : Strings.t("quit.idle")
        confirmText: Strings.t("common.quit")
        onAccepted: Qt.quit()
    }
}
