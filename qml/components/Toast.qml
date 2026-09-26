// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0
import "."

// 轻提示：页面顶部居中浮现 1.6s 后自动淡出。show() 触发。
Popup {
    id: toast
    property string text: ""
    property int durationMs: 1600

    function show() {
        open();
        hideTimer.restart();
    }

    anchors.centerIn: Overlay.overlay
    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: 10

    background: Rectangle {
        radius: 6
        color: Theme.alpha(Theme.secondaryText, 0.9)
    }
    contentItem: UiText {
        text: toast.text
        color: "#FFFFFF"
        font.pixelSize: Theme.captionSize
    }

    Timer {
        id: hideTimer
        interval: toast.durationMs
        onTriggered: toast.close()
    }
}
