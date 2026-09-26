// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0
import "."

// 统一复选框：可见方框 18×18（选中主色底+白勾，勾用统一矢量资源）。
// padding 扩大点击热区而不放大可见方框；标准 CheckBox 保留键盘/三态语义。
CheckBox {
    id: control

    // 热区：18 方框 + 两侧 8 padding = 34×34 可点区域。
    padding: 8
    spacing: 8
    opacity: enabled ? 1.0 : 0.55
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: control.leftPadding
        y: control.height / 2 - height / 2
        radius: 4
        color: control.checkState === Qt.Checked ? Theme.primaryButton : "transparent"
        border.width: control.checkState === Qt.PartiallyChecked ? 2 : 1.5
        border.color: {
            if (control.checkState !== Qt.Unchecked) return Theme.primaryButton;
            if (!control.enabled) return Theme.divider;
            return control.hovered ? Theme.alpha(Theme.secondaryText, 0.7)
                                   : Theme.alpha(Theme.secondaryText, 0.45);
        }
        Behavior on border.color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
        Behavior on color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }

        // 半选态短横线。
        Rectangle {
            visible: control.checkState === Qt.PartiallyChecked
            anchors.centerIn: parent
            width: 8; height: 2
            radius: 1
            color: "#FFFFFF"
        }

        // 选中勾：统一矢量资源（UiIcon check），白色。
        UiIcon {
            visible: control.checkState === Qt.Checked
            anchors.centerIn: parent
            width: 12; height: 12
            strokeWidth: 2.6
            glyph: "check"
            ink: "#FFFFFF"
            // 出现动画：轻缩放。
            scale: visible ? 1.0 : 0.5
            Behavior on scale { NumberAnimation { duration: Theme.reduceMotion ? 0 : 90 } }
        }
    }

    contentItem: UiText {
        leftPadding: control.indicator.width + control.spacing
        verticalAlignment: Text.AlignVCenter
        text: control.text
        visible: control.text.length > 0
        color: Theme.text
    }
}
