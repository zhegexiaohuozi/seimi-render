// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 表单字段标签：可选 CLI 覆盖徽标（CLI 显式 flag 的字段不可改，GUI 显示成因）。
ColumnLayout {
    id: root
    property string text: ""
    property string cliBadgeText: ""
    spacing: 2

    signal editBlocked()

    RowLayout {
        spacing: 6
        Layout.fillWidth: true
        UiText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: root.text
            color: Theme.secondaryText
            font.pixelSize: Theme.captionSize
        }
        StatusBadge {
            visible: root.cliBadgeText.length > 0
            text: root.cliBadgeText
            tone: Theme.warning
        }
    }
}
