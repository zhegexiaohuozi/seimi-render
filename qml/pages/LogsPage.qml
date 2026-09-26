// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "../components"

// 日志页：过滤器工具栏 + 虚拟化列表（LogModel，1s 拉取环形缓冲）。
// 列表高度绑定页面可视高度（外壳 Main.pageViewportHeight），内滚不套外滚。
ColumnLayout {
    id: page
    spacing: Theme.gap

    function levelColor(levelSort) {
        return levelSort === 4 || levelSort === 3 ? Theme.danger
            : levelSort === 2 ? Theme.warning
            : levelSort === 0 ? Theme.secondaryText
            : Theme.primary;
    }

    // —— 工具栏 ——
    RowLayout {
        Layout.fillWidth: true
        spacing: 10

        StatusBadge { text: Logs.visibleCount + " " + Strings.t("logs.count") }
        Item { Layout.fillWidth: true }

        UiTextField {
            id: searchField
            width: 200
            placeholderText: Strings.t("logs.search-ph")
            onTextChanged: Logs.filterText = text
        }
        UiCheckBox {
            text: Strings.t("logs.debug")
            checked: Logs.showDebug
            onToggled: Logs.showDebug = checked
        }
        UiCheckBox {
            text: Strings.t("logs.warning")
            checked: Logs.showWarning
            onToggled: Logs.showWarning = checked
        }
        UiCheckBox {
            text: Strings.t("logs.critical")
            checked: Logs.showCritical
            onToggled: Logs.showCritical = checked
        }
        UiSwitch {
            id: autoScrollSwitch
            text: Strings.t("logs.autoscroll")
            checked: true
        }
        UiButton { text: Strings.t("logs.clear"); onClicked: Logs.clear() }
        UiButton { text: Strings.t("logs.open-dir"); onClicked: ServiceInfo.openDataDir() }
    }

    // —— 列表 ——
    Rectangle {
        Layout.fillWidth: true
        // 视口高度 - 工具栏与内边距余量；下限 320。
        Layout.preferredHeight: Math.max(320, Window.window.pageViewportHeight - 110)
        radius: Theme.cardRadius
        color: Theme.card
        border.width: 1
        border.color: Theme.divider

        ListView {
            id: logList
            anchors.fill: parent
            anchors.margins: 8
            clip: true
            model: Logs
            ScrollBar.vertical: ScrollBar {}

            // 自动滚动：仅在用户本来就在底部时跟随新日志（向上翻阅不打扰）。
            onCountChanged: {
                if (autoScrollSwitch.checked && atYEnd) {
                    positionViewAtEnd();
                }
            }

            delegate: RowLayout {
                width: logList.width - 16
                spacing: 8

                UiText {
                    text: time
                    color: Theme.secondaryText
                    font.pixelSize: Theme.captionSize
                    font.family: Theme.monoFamily
                    Layout.preferredWidth: 62
                }
                Rectangle {
                    Layout.preferredWidth: levelChip.implicitWidth + 12
                    implicitHeight: 16
                    radius: 3
                    color: Theme.alpha(page.levelColor(levelSort), 0.14)
                    UiText {
                        id: levelChip
                        anchors.centerIn: parent
                        text: level
                        color: page.levelColor(levelSort)
                        font.pixelSize: Theme.captionSize
                        font.family: Theme.monoFamily
                    }
                }
                UiText {
                    text: message
                    font.pixelSize: Theme.captionSize
                    font.family: Theme.monoFamily
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 1000
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }

            UiText {
                visible: Logs.visibleCount === 0
                anchors.centerIn: parent
                text: Strings.t("logs.empty")
                color: Theme.secondaryText
            }
        }
    }
}
