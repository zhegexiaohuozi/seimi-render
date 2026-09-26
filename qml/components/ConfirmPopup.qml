// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "."

// 确认弹层（Modal）：标题 + 正文 + 取消/确认。danger 时确认键红色。
// Escape 可关（Popup 默认 closePolicy），无键盘陷阱。
Popup {
    id: popup

    property string title: ""
    property string message: ""
    property string confirmText: ""
    property bool danger: false
    signal accepted()

    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: Theme.cardRadius
        color: Theme.card
        border.width: 1
        border.color: Theme.divider
    }

    contentItem: ColumnLayout {
        spacing: 14
        UiText {
            text: popup.title
            font.pixelSize: Theme.cardTitleSize
            font.weight: Font.DemiBold
            Layout.fillWidth: true
        }
        UiText {
            text: popup.message
            color: Theme.secondaryText
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            Layout.fillWidth: true
            Layout.preferredWidth: 360
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 10
            UiButton {
                text: Strings.t("common.cancel")
                onClicked: popup.close()
            }
            UiButton {
                variant: "filled"
                danger: popup.danger
                text: popup.confirmText !== "" ? popup.confirmText : Strings.t("common.confirm")
                onClicked: {
                    popup.accepted();
                    popup.close();
                }
            }
        }
    }
}
