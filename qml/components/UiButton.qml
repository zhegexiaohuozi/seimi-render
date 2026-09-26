// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0
import "."

// 统一按钮：filled / outlined / quiet 三变体；高度 32、圆角 4（任务书规格）。
// 基于 Button（保留键盘/焦点/禁用行为），只定制背景与内容。
Button {
    id: control
    property string variant: "outlined"
    property bool danger: false
    implicitHeight: 32
    implicitWidth: Math.max(70, label.implicitWidth + 24)
    leftPadding: 12; rightPadding: 12
    topPadding: 5; bottomPadding: 5
    hoverEnabled: true
    font.family: Theme.fontFamily
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.45
    contentItem: UiText {
        id: label
        text: control.text
        font: control.font
        color: control.variant === "filled"
               ? "#FFFFFF"
               : (control.danger ? Theme.danger : Theme.primary)
        horizontalAlignment: Text.AlignHCenter
    }
    background: Rectangle {
        radius: 4
        color: control.variant === "filled"
               ? (control.down ? Qt.darker(control.danger ? Theme.danger : Theme.primaryButton, 1.1)
                               : (control.danger ? Theme.danger : Theme.primaryButton))
               : ((control.down || control.hovered)
                    ? Theme.alpha(control.danger ? Theme.danger : Theme.primary, 0.08)
                    : "transparent")
        border.width: control.visualFocus || control.variant === "outlined" ? 1 : 0
        border.color: control.danger ? Theme.danger : Theme.primary
        Behavior on color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
    }
}
