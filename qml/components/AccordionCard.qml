// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 折叠列表卡（登录态域名行）：整行点击展开，图标 + 标题 + 徽标 + 副题 + 箭头。
// 展开区放详情与操作；操作按钮与整行点击不冲突（子项事件优先于 delegate）。
Control {
    id: root
    property string title: ""
    property string subtitle: ""
    property string badge: ""
    property string glyph: "lock"
    property color accent: Theme.primary
    property bool expanded: false
    default property alias details: detailColumn.data
    padding: 0
    implicitHeight: 68 + (expanded ? detailColumn.implicitHeight + 24 : 0)
    background: Rectangle { color: Theme.card; radius: Theme.cardRadius }
    contentItem: Item {
        ItemDelegate {
            id: trigger
            anchors.left: parent.left
            anchors.right: parent.right
            height: 68
            leftPadding: 16; rightPadding: 16
            topPadding: 10; bottomPadding: 10
            hoverEnabled: true
            Accessible.name: root.title
            onClicked: root.expanded = !root.expanded
            background: Rectangle {
                radius: Theme.cardRadius
                color: trigger.hovered ? Theme.hover : "transparent"
                border.width: trigger.visualFocus ? 1 : 0
                border.color: Theme.primary
            }
            contentItem: RowLayout {
                spacing: 14
                UiIcon { glyph: root.glyph; ink: root.accent; Layout.preferredWidth: 30; Layout.preferredHeight: 30 }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 4
                    UiText { Layout.fillWidth: true; text: root.title; font.pixelSize: 16; font.weight: Font.Bold }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        StatusBadge { visible: root.badge.length > 0; text: root.badge }
                        UiText {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: root.subtitle
                            color: Theme.secondaryText
                            font.pixelSize: Theme.captionSize
                        }
                    }
                }
                UiIcon {
                    glyph: "chevron"
                    Layout.preferredWidth: 20; Layout.preferredHeight: 20
                    rotation: root.expanded ? 180 : 0
                    Behavior on rotation { NumberAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
                }
            }
        }
        ColumnLayout {
            id: detailColumn
            visible: root.expanded
            anchors.top: trigger.bottom
            anchors.topMargin: 8
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 8
        }
    }
}
