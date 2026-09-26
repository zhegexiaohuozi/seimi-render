// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 设置行：左侧主标签（主文字色，可伸缩省略）→ 右侧控件（field 显式分配，限宽右对齐）。
// 辅助说明 hint（浅灰）/ 警告 warning（警示色）各占独立行。
// 用法：SettingRow { label: "…"; field: UiTextField { width: 110 }; hint: "…" }
// 控件不 fillWidth：宽度由控件自身 width/implicitWidth 决定（端口 110 / 地址 220 / 开关 42…）。
ColumnLayout {
    id: row

    property string label: ""
    property string cliBadgeText: ""
    property var field: null
    property string hint: ""
    property string warning: ""

    spacing: 2
    Layout.fillWidth: true

    onFieldChanged: mountField()
    Component.onCompleted: mountField()
    function mountField() {
        if (field) fieldSlot.data = [field];
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        Layout.topMargin: 8
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 6
            UiText {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: row.label
                elide: Text.ElideRight
            }
            StatusBadge {
                visible: row.cliBadgeText.length > 0
                text: row.cliBadgeText
                tone: Theme.warning
            }
        }

        // 控件槽：右对齐，尺寸随控件自身（绝不被 Layout 拉伸）。
        Item {
            id: fieldSlot
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Layout.preferredWidth: childrenRect.width
            Layout.preferredHeight: childrenRect.height
        }
    }

    UiText {
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        visible: row.hint.length > 0
        text: row.hint
        color: Theme.secondaryText
        font.pixelSize: Theme.captionSize
        wrapMode: Text.WordWrap
        maximumLineCount: 2
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        Layout.bottomMargin: 4
        visible: row.warning.length > 0
        spacing: 6
        UiIcon {
            glyph: "alert"
            width: 13; height: 13
            ink: Theme.danger
            Layout.alignment: Qt.AlignTop
        }
        UiText {
            Layout.fillWidth: true
            text: row.warning
            color: Theme.danger
            font.pixelSize: Theme.captionSize
            wrapMode: Text.WordWrap
            maximumLineCount: 2
        }
    }
    // 无 hint/警告时底部留白，与首行 topMargin 对称。
    Item { visible: row.hint.length === 0 && row.warning.length === 0; height: 8 }
}
