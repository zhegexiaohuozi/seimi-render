// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SeimiRender.Gui 1.0

// 占比条（ProgressBar 定制）：只在存在真实分子/分母时表达负载；0 填充为零。
ProgressBar {
    id: root
    from: 0; to: 1; value: 0
    padding: 0
    implicitHeight: 6
    background: Rectangle {
        radius: 3
        color: Theme.alpha(Theme.primary, 0.14)
    }
    contentItem: Item {
        Rectangle {
            width: parent.width * root.position
            height: parent.height
            radius: 3
            color: Theme.primary
        }
    }
}
