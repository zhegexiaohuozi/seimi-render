// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SeimiRender.Gui 1.0

// Font Awesome Free 固体图标（官方原始 SVG，qrc:/third_party/FontAwesome/，由 C++
// FaIconProvider 光栅化+着色，见 src/gui/FaIconProvider.h）。
// 尺寸随自身 width/height（等比居中，禁止非等比拉伸、无父级 scale）；glyphInset
// 为四周光学内缩（默认 1：导航 24×24 槽 → 22×22 字形，符合统一规格）。
// 着色经 URL 参数（aarrggbb）传入 provider——Image 无 color 属性，主题/状态色
// 统一走 tint；DPR 同经 URL 显式传入（provider 是唯一光栅化方，无重复乘算），
// 跨 DPI 显示器移动时 Screen.devicePixelRatio 变化自动重光栅。
Item {
    id: root
    property string name: ""              // FA 图标名 = 文件名（"house" / "arrows-rotate"…）
    property color tint: Theme.text
    property real glyphInset: 1
    implicitWidth: 24
    implicitHeight: 24

    function hex8(c) {
        function h2(v) { var s = Math.round(v * 255).toString(16); return s.length === 1 ? "0" + s : s }
        return h2(c.a) + h2(c.r) + h2(c.g) + h2(c.b)
    }

    Image {
        anchors.centerIn: parent
        width: Math.max(1, root.width - 2 * root.glyphInset)
        height: Math.max(1, root.height - 2 * root.glyphInset)
        fillMode: Image.PreserveAspectFit
        sourceSize: Qt.size(width, height)
        source: root.name
            ? "image://fa/" + root.name + "/" + root.hex8(root.tint) + "/" + Screen.devicePixelRatio
            : ""
    }
}
