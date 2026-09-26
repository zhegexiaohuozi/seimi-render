// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SeimiRender.Gui 1.0

// 统一文本：默认主题字体/字号/PlainText/单行省略。
// 放开多行时由调用方设 wrapMode/maximumLineCount。
Text {
    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize
    font.weight: Font.Normal
    renderType: Text.QtRendering
    textFormat: Text.PlainText
    verticalAlignment: Text.AlignVCenter
    elide: Text.ElideRight
    maximumLineCount: 1
}
