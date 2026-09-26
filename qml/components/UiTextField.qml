// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 统一文本输入：高 38 / 圆角 4 / 左右留白 12（任务书规格）。
// 状态：普通/悬停/聚焦/错误/禁用/只读——边框恒 1px 只换色，杜绝状态切换抖动。
// 宽度由调用方给定（端口 100-120、地址 200-240），默认不 fillWidth。
TextField {
    id: field

    property bool errorState: false          // 业务校验失败（如 0.0.0.0 暴露等场景）
    property real fieldHeight: 38

    implicitHeight: fieldHeight
    leftPadding: 12; rightPadding: 12
    topPadding: 6; bottomPadding: 6
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize
    color: enabled ? Theme.text : Theme.secondaryText
    placeholderTextColor: Theme.secondaryText
    selectionColor: Theme.alpha(Theme.primary, 0.35)
    selectedTextColor: Theme.text
    verticalAlignment: TextInput.AlignVCenter
    opacity: enabled || readOnly ? 1.0 : 0.55

    selectByMouse: true

    background: Rectangle {
        implicitHeight: field.fieldHeight
        radius: 4
        color: readOnly ? Theme.inset : Theme.card
        border.width: 1
        border.color: {
            if (!field.enabled) return Theme.divider;
            if (field.errorState) return Theme.danger;
            if (field.activeFocus) return Theme.primary;
            if (field.hovered) return Theme.alpha(Theme.secondaryText, 0.55);
            return Theme.divider;
        }
        Behavior on border.color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
    }
}
