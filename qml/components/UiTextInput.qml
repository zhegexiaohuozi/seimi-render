// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SeimiRender.Gui 1.0

// 裸 TextInput（SpinBox 等控件的 contentItem 用）：主题字体/选区色/PlainText 语义。
TextInput {
    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.bodySize
    selectionColor: Theme.alpha(Theme.primary, 0.35)
    selectedTextColor: Theme.text
    renderType: TextInput.QtRendering
    selectByMouse: true
}
