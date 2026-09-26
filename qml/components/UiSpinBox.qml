// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0
import "."

// 统一数字输入：收束为 132 宽完整控件（±按钮内嵌右侧），不再被 Layout 拉满整行。
// 保留标准 SpinBox 范围/步进/键盘上下键/滚轮/校验语义（editable 由调用方控制）。
SpinBox {
    id: spin

    property real fieldHeight: 38

    implicitWidth: 132
    implicitHeight: fieldHeight
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize
    opacity: enabled ? 1.0 : 0.55

    // 数值文本居中（镜像 displayText；text 在构建期为 undefined 会告警）。
    contentItem: UiTextInput {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 44
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        text: spin.displayText
        font: spin.font
        color: spin.enabled ? Theme.text : Theme.secondaryText
        selectionColor: Theme.alpha(Theme.primary, 0.35)
        selectedTextColor: Theme.text

        readOnly: !spin.editable
        validator: spin.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
    }

    // 减号：右列上/下两个 26 宽热区，矢量 chevron。
    up.indicator: Rectangle {
        x: spin.width - width - 4
        y: 4
        width: 30; height: (spin.height - 8) / 2
        radius: 4
        color: spin.up.hovered ? Theme.hover : "transparent"
        UiIcon {
            anchors.centerIn: parent
            width: 12; height: 12
            glyph: "chevron"
            rotation: 0            // 上箭头 = chevron 原向（尖朝上）
            ink: spin.up.hovered || spin.up.pressed ? Theme.primary : Theme.secondaryText
        }
    }
    down.indicator: Rectangle {
        x: spin.width - width - 4
        y: spin.height / 2
        width: 30; height: (spin.height - 8) / 2
        radius: 4
        color: spin.down.hovered ? Theme.hover : "transparent"
        UiIcon {
            anchors.centerIn: parent
            width: 12; height: 12
            glyph: "chevron"
            rotation: 180          // 下箭头
            ink: spin.down.hovered || spin.down.pressed ? Theme.primary : Theme.secondaryText
        }
    }

    background: Rectangle {
        implicitHeight: spin.fieldHeight
        radius: 4
        color: Theme.card
        border.width: 1
        border.color: {
            if (!spin.enabled) return Theme.divider;
            if (spin.activeFocus) return Theme.primary;
            if (spin.hovered) return Theme.alpha(Theme.secondaryText, 0.55);
            return Theme.divider;
        }
        Behavior on border.color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
        // 按钮列与数值区的竖分隔线。
        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 38
            anchors.top: parent.top
            anchors.topMargin: 7
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 7
            width: 1
            color: Theme.divider
        }
    }
}
