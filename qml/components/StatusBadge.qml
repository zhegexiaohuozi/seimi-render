// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0
import "."

// 状态徽标：tone 颜色 + 可选浅底。文字永不承载关键信息以外的内容。
Control {
    id: root
    property string text: ""
    property color tone: Theme.primary
    property bool filled: false
    implicitWidth: label.implicitWidth + leftPadding + rightPadding
    implicitHeight: label.implicitHeight + topPadding + bottomPadding
    padding: 4
    leftPadding: 6; rightPadding: 6
    topPadding: 2; bottomPadding: 2
    contentItem: UiText {
        id: label
        text: root.text
        color: root.tone
        font.pixelSize: 11
    }
    background: Rectangle {
        radius: 4
        color: root.filled ? Theme.alpha(root.tone, 0.10) : "transparent"
        border.width: 1
        border.color: Theme.alpha(root.tone, 0.55)
    }
}
