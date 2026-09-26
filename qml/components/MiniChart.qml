// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SeimiRender.Gui 1.0
import "."

// 低复杂度数据驱动折线图（inset 底 + 网格线）。
// 只画真实历史样本；不足 2 个样本显示空状态，不画假曲线。
Rectangle {
    id: root
    property var samples: []
    property string emptyText: ""
    property color lineColor: Theme.primary
    color: Theme.inset
    radius: 8
    implicitHeight: 140
    border.width: 1
    border.color: Theme.divider
    onSamplesChanged: graph.requestPaint()
    Canvas {
        id: graph
        anchors.fill: parent
        anchors.margins: 12
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections {
            target: Theme
            function onColorsChanged() { graph.requestPaint(); }
        }
        onPaint: {
            const ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            ctx.strokeStyle = Theme.divider;
            ctx.lineWidth = 1;
            for (let row = 0; row < 4; ++row) {
                const yy = 8 + (height - 16) * row / 3;
                ctx.beginPath(); ctx.moveTo(0, yy); ctx.lineTo(width, yy); ctx.stroke();
            }
            const values = root.samples.filter(v => Number.isFinite(v) && v >= 0);
            if (values.length < 2) return;
            const maximum = Math.max(1, ...values);
            ctx.strokeStyle = root.lineColor;
            ctx.lineWidth = 1.8;
            ctx.beginPath();
            for (let i = 0; i < values.length; ++i) {
                const xx = width * i / (values.length - 1);
                const yy = height - 8 - (height - 16) * values[i] / maximum;
                if (i === 0) ctx.moveTo(xx, yy); else ctx.lineTo(xx, yy);
            }
            ctx.stroke();
        }
    }
    UiText {
        anchors.centerIn: parent
        width: parent.width - 24
        horizontalAlignment: Text.AlignHCenter
        color: Theme.secondaryText
        font.pixelSize: Theme.captionSize
        visible: root.samples.length < 2
        text: root.emptyText
    }
}
