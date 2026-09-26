// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0
import "."

// 统一下拉：外框/显示文字/箭头/popup/delegate 全量定制（与 UiTextField 同几何）。
// 保留标准 ComboBox 键盘（方向键/Enter/Escape）与高亮行为。
ComboBox {
    id: combo

    property real fieldHeight: 38

    implicitHeight: fieldHeight
    implicitWidth: 160
    leftPadding: 12
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize
    opacity: enabled ? 1.0 : 0.55

    // QQC2 Popup 打开时挂到窗口 Overlay，不随本控件/所在页面的 visible 一起隐藏。
    // 页面切走（预实例化页面 visible=false）或禁用时必须显式收起，否则弹层残留为
    // “孤立菜单”（仍拦截点击）。close() 对已关闭弹层是幂等空操作。
    onVisibleChanged: if (!visible) popup.close()
    onEnabledChanged: if (!enabled) popup.close()

    // —— delegate（popup ListView 经 delegateModel 复用；键盘高亮联动）——
    delegate: ItemDelegate {
        id: item
        required property var model
        required property int index
        width: combo.width - 8
        height: 32
        hoverEnabled: true
        highlighted: combo.highlightedIndex === item.index

        contentItem: UiText {
            leftPadding: 8
            verticalAlignment: Text.AlignVCenter
            text: {
                const m = item.model;
                if (m === undefined || m === null) return "";
                if (combo.textRole !== "" && m[combo.textRole] !== undefined) return m[combo.textRole];
                return m.modelData !== undefined ? m.modelData : String(m);
            }
            color: (item.highlighted || item.hovered) ? Theme.primary : Theme.text
        }
        background: Rectangle {
            radius: 4
            color: item.highlighted ? Theme.alpha(Theme.primary, 0.12)
                   : item.hovered ? Theme.hover : "transparent"
        }
        onClicked: combo.currentIndex = item.index
    }

    // —— 显示文字 ——
    contentItem: UiText {
        leftPadding: 0
        rightPadding: 28
        verticalAlignment: Text.AlignVCenter
        text: combo.displayText
        color: combo.enabled ? Theme.text : Theme.secondaryText
    }

    // —— 箭头（统一矢量资源，随展开旋转）——
    indicator: UiIcon {
        x: combo.width - width - 10
        y: (combo.height - height) / 2
        width: 16; height: 16
        glyph: "chevron"
        ink: Theme.secondaryText
        rotation: combo.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
    }

    // —— 外框：与 UiTextField 状态族一致 ——
    background: Rectangle {
        implicitHeight: combo.fieldHeight
        radius: 4
        color: Theme.card
        border.width: 1
        border.color: {
            if (!combo.enabled) return Theme.divider;
            if (combo.popup.visible) return Theme.primary;
            if (combo.hovered) return Theme.alpha(Theme.secondaryText, 0.55);
            return Theme.divider;
        }
        Behavior on border.color { ColorAnimation { duration: Theme.reduceMotion ? 0 : Theme.motionMs } }
    }

    // —— 弹层：卡片底/圆角/项悬停与选中态 ——
    popup: Popup {
        y: combo.height + 4
        width: combo.width
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 320)

        background: Rectangle {
            radius: Theme.cardRadius
            color: Theme.card
            border.width: 1
            border.color: Theme.divider
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            spacing: 2
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        }
    }
}
