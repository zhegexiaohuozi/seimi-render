// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0

// 统一开关：42×26 轨道 / 22×22 白圆点 / 内缩 2 / 位移 16。
// 关闭态完整灰色胶囊、开启态主题色；圆点完全内嵌（不是细线轨道+外凸大圆）。
// 标准 Switch + visualPosition：保留键盘（Space）、拖动、可访问性。
Switch {
    id: control

    implicitWidth: 42
    implicitHeight: 26
    spacing: 8
    opacity: enabled ? 1.0 : 0.55

    indicator: Rectangle {
        implicitWidth: 42
        implicitHeight: 26
        radius: 13
        // 关闭态实心灰胶囊；开启态主题色。过渡用色动画（不缩放轨道）。
        color: control.checked ? Theme.primaryButton
                               : Theme.alpha(Theme.secondaryText, 0.35)
        Behavior on color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs * 1.5 } }

        Rectangle {
            // x = 2 + visualPosition * 16（42 - 2*2 - 22 = 16 位移量）。
            x: 2 + control.visualPosition * 16
            y: 2
            width: 22; height: 22
            radius: 11
            color: "#FFFFFF"
            // 按压轻微收缩反馈（比例缩放，不改几何规则）。
            scale: control.pressed ? 0.92 : 1.0
            Behavior on scale { NumberAnimation { duration: Theme.reduceMotion ? 0 : 80 } }
        }
    }

    // 文字标签（如有）。
    contentItem: UiText {
        leftPadding: control.indicator.width + control.spacing
        verticalAlignment: Text.AlignVCenter
        text: control.text
        visible: control.text.length > 0
        color: Theme.text
    }
}
