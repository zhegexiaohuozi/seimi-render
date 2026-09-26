// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SeimiRender.Gui 1.0

// 统一图标（Font Awesome Free 官方 SVG，经 FaIcon/FaIconProvider 光栅化+着色）。
// 兼容旧 API：glyph 为历史语义名（映射到 FA 文件名），strokeWidth 仅为兼容保留、
// 无描边语义（FA solid 为实心填充）。未映射的 glyph 直接按 FA 文件名解析——
// 新代码建议直接传 FA 名（"server" / "chevron-down"…）。
Item {
    id: root
    property string glyph: "house"
    property color ink: Theme.text
    property real strokeWidth: 1.8   // 兼容旧调用点（UpdatePage/UiCheckBox），已无效果
    property real glyphInset: 1
    implicitWidth: 24
    implicitHeight: 24

    // 历史 glyph 名 → FA 6 文件名（导航语义名 + 业务名 + 窗口控件弃用名）。
    readonly property var faMap: ({
        "home": "house", "settings": "sliders", "update": "arrows-rotate",
        "logs": "file-lines", "lock": "lock",
        "server": "server", "network": "network-wired", "chart": "chart-line",
        "chevron": "chevron-down", "check": "check", "clock": "clock",
        "link": "link", "cpu": "microchip", "globe": "globe",
        "alert": "triangle-exclamation", "moon": "moon", "sun": "sun"
    })

    FaIcon {
        anchors.fill: parent
        glyphInset: root.glyphInset
        name: root.faMap[root.glyph] !== undefined ? root.faMap[root.glyph] : root.glyph
        tint: root.ink
    }
}
