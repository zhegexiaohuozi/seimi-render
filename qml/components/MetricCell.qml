// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import "."

// 紧凑指标格：图标 + 标签 + 数值/单位。数值缺失传 "—"（不许用 0/100% 冒充）。
Rectangle {
    id: root
    property string label: ""
    property string value: "—"
    property string unit: ""
    property string glyph: "chart"
    property color tone: Theme.primary
    implicitHeight: 66
    implicitWidth: 160
    radius: 8
    color: Theme.alpha(tone, 0.05)
    border.width: 1
    border.color: Theme.alpha(tone, 0.15)
    RowLayout {
        anchors.fill: parent
        anchors.margins: 9
        spacing: 10
        Rectangle {
            Layout.preferredWidth: 32; Layout.preferredHeight: 32
            radius: 16
            color: Theme.alpha(root.tone, 0.10)
            UiIcon {
                anchors.centerIn: parent
                width: 20; height: 20
                glyph: root.glyph; ink: root.tone
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 2
            UiText {
                Layout.fillWidth: true
                text: root.label
                color: Theme.secondaryText
                font.pixelSize: Theme.captionSize
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                UiText { Layout.minimumWidth: 0; text: root.value; font.pixelSize: 17; font.weight: Font.Bold }
                UiText { Layout.fillWidth: true; text: root.unit; font.pixelSize: 11; color: Theme.secondaryText }
            }
        }
    }
}
