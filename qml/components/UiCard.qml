// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 统一卡片：图标底座(38/r6) + 标题(18/500) + 右侧操作 + 1px 分隔线 + 内容槽。
// 无描边无阴影——分层背景（card 落在 canvas 上）区分模块。
Control {
    id: root
    property string title: ""
    property string subtitleText: ""   // 标题右侧辅助小字（可选；域名卡「按请求数排序」用）
    property string glyph: "server"
    property color accent: Theme.primary
    property string actionText: ""
    property string badgeText: ""
    property color badgeTone: Theme.secondaryText
    signal actionTriggered()
    default property alias body: bodyLayout.data
    padding: 0
    implicitWidth: 330
    implicitHeight: Theme.cardHeaderHeight + 1 + Theme.cardPadding * 2 + bodyLayout.implicitHeight
    background: Rectangle { radius: Theme.cardRadius; color: Theme.card }
    contentItem: Item {
        id: cardInner
        Item {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Theme.cardHeaderHeight
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.cardPadding
                anchors.rightMargin: Theme.cardPadding
                spacing: 12
                Rectangle {
                    Layout.preferredWidth: Theme.iconBoxSize
                    Layout.preferredHeight: Theme.iconBoxSize
                    radius: 6
                    color: Theme.alpha(root.accent, 0.12)
                    UiIcon {
                        anchors.centerIn: parent
                        width: 24; height: 24
                        glyph: root.glyph
                        ink: root.accent
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    UiText {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.title
                        font.pixelSize: Theme.cardTitleSize
                        font.weight: Font.Medium
                    }
                    UiText {
                        visible: root.subtitleText.length > 0
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.subtitleText
                        color: Theme.secondaryText
                        font.pixelSize: Theme.captionSize
                    }
                }
                StatusBadge {
                    visible: root.badgeText.length > 0
                    text: root.badgeText
                    tone: root.badgeTone
                }
                UiButton {
                    visible: root.actionText.length > 0
                    text: root.actionText
                    onClicked: root.actionTriggered()
                }
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            height: 1
            color: Theme.divider
        }
        ColumnLayout {
            id: bodyLayout
            anchors.top: header.bottom
            anchors.topMargin: Theme.cardPadding + 1
            anchors.bottom: cardInner.bottom
            anchors.bottomMargin: Theme.cardPadding
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.cardPadding
            anchors.rightMargin: Theme.cardPadding
            spacing: Theme.gap
        }
    }
}
