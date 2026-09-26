// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 设置分组卡：无大图标底座、无贯穿分隔线（区别于概览的 UiCard）。
// 结构 = 纯文字组标题 + 行列表。整卡同底色圆角，行间距 2，行内上下留白由 SettingRow 承担。
Control {
    id: section

    property string title: ""
    property string badgeText: ""
    default property alias rows: rowsLayout.data

    padding: 0
    implicitWidth: 360
    implicitHeight: body.implicitHeight

    background: Rectangle {
        radius: Theme.cardRadius
        color: Theme.card
    }

    contentItem: ColumnLayout {
        id: body
        spacing: 2

        // 组标题行：纯文字（主文字色 Medium 15px），可选右侧徽标（重启生效/立即生效）。
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.cardPadding
            Layout.rightMargin: Theme.cardPadding
            Layout.topMargin: 12
            Layout.bottomMargin: 6
            spacing: 8

            UiText {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: section.title
                font.pixelSize: 15
                font.weight: Font.Medium
            }
            StatusBadge {
                visible: section.badgeText.length > 0
                text: section.badgeText
            }
        }

        ColumnLayout {
            id: rowsLayout
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            Layout.bottomMargin: 6
            spacing: 2
        }
    }
}
