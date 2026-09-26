// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 侧栏导航项（ItemDelegate 基底）：高 48、左右外边距 10 由父级布局承担。
// 选中态 navSelected 底色 + 原文字色（浅色为深字，不用蓝字）。
ItemDelegate {
    id: control
    property string glyph: "house"
    property bool selected: false
    implicitHeight: Theme.navHeight
    implicitWidth: Theme.sidebarWidth - 20
    leftPadding: 14; rightPadding: 14
    topPadding: 10; bottomPadding: 10
    hoverEnabled: true
    Accessible.name: text
    contentItem: RowLayout {
        spacing: 12
        FaIcon {
            name: control.glyph
            Layout.preferredWidth: 24; Layout.preferredHeight: 24
        }
        UiText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: control.text
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 16
            font.weight: Font.Bold
            font.letterSpacing: text.length === 2 ? 3 : 1
        }
    }
    background: Rectangle {
        radius: Theme.cardRadius
        color: control.selected ? Theme.navSelected
               : (control.down || control.hovered ? Theme.hover : "transparent")
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.primary
        Behavior on color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
    }
}
