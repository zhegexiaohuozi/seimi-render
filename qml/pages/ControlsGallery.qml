// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "../components"

// 控件状态陈列室（--ui-demo 入口，验收用）：集中展示所有定制控件的
// 深浅主题 × 普通/悬停代用/聚焦/错误/禁用/只读状态与交互。
// 数据全部本地演示值并明确标注；加载 800ms 后自动展开一个下拉（验收展开态截图）。
ColumnLayout {
    id: gallery
    spacing: Theme.gap

    readonly property string demoNote: Strings.t("gallery.demo-note")
    // 自动展开只在本页首次实际可见后触发一次。曾为无条件 running:true——普通启动
    // 本页不可见但 Timer 照跑，popup.open() 把弹层挂上窗口 Overlay（不随父级隐藏），
    // 成为概览页左下角的“直连/HTTP/SOCKS5”孤立菜单。可见性门控 + 一次性标志根治。
    property bool autoDemoShown: false

    StatusBadge { text: demoNote; tone: Theme.warning; filled: true }

    // ============ 文本输入 ============
    SettingsSection {
        Layout.fillWidth: true
        title: Strings.t("gallery.textfield")

        SettingRow {
            label: Strings.t("gallery.st-normal")
            field: UiTextField { width: 220; placeholderText: "127.0.0.1" }
        }
        SettingRow {
            label: Strings.t("gallery.st-filled")
            field: UiTextField { width: 220; text: "example.com/path" }
        }
        SettingRow {
            label: Strings.t("gallery.st-focus")
            field: UiTextField { width: 220; text: "8088"; activeFocusOnTab: true; Component.onCompleted: forceActiveFocus() }
        }
        SettingRow {
            label: Strings.t("gallery.st-error")
            field: UiTextField { width: 220; text: "0.0.0.0"; errorState: true }
            hint: Strings.t("gallery.st-error-hint")
        }
        SettingRow {
            label: Strings.t("gallery.st-disabled")
            field: UiTextField { width: 220; text: "disabled"; enabled: false }
        }
        SettingRow {
            label: Strings.t("gallery.st-readonly")
            field: UiTextField { width: 220; text: "read-only value"; readOnly: true }
        }
    }

    // ============ 数字输入 ============
    SettingsSection {
        Layout.fillWidth: true
        title: Strings.t("gallery.spinbox")

        SettingRow {
            label: Strings.t("gallery.st-normal")
            field: UiSpinBox { from: 0; to: 65535; value: 8088; editable: true }
        }
        SettingRow {
            label: Strings.t("gallery.st-large")
            field: UiSpinBox { from: 1000; to: 600000; stepSize: 1000; value: 30000; editable: true }
        }
        SettingRow {
            label: Strings.t("gallery.st-disabled")
            field: UiSpinBox { from: 0; to: 64; value: 12; enabled: false }
        }
    }

    // ============ 开关 / 复选 ============
    SettingsSection {
        Layout.fillWidth: true
        title: Strings.t("gallery.toggles")

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 4
            spacing: 28

            ColumnLayout { spacing: 8
                UiText { text: Strings.t("gallery.sw-on"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                UiSwitch { checked: true }
            }
            ColumnLayout { spacing: 8
                UiText { text: Strings.t("gallery.sw-off"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                UiSwitch { checked: false }
            }
            ColumnLayout { spacing: 8
                UiText { text: Strings.t("gallery.sw-disabled"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                UiSwitch { checked: true; enabled: false }
            }
            Item { Layout.fillWidth: true }
            ColumnLayout { spacing: 8
                UiText { text: Strings.t("gallery.cb-group"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                UiCheckBox { text: Strings.t("gallery.cb-checked"); checked: true }
                UiCheckBox { text: Strings.t("gallery.cb-unchecked"); checked: false }
                UiCheckBox { text: Strings.t("gallery.st-disabled"); checked: true; enabled: false }
            }
        }
    }

    // ============ 下拉 ============
    SettingsSection {
        Layout.fillWidth: true
        title: Strings.t("gallery.combobox")

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 4
            Layout.bottomMargin: 8
            spacing: 24

            ColumnLayout { spacing: 8
                UiText { text: Strings.t("gallery.st-normal"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                UiComboBox { id: demoCombo; model: [Strings.t("settings.direct"), "HTTP", "SOCKS5"]; width: 180 }
            }
            ColumnLayout { spacing: 8
                UiText { text: Strings.t("gallery.st-disabled"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                UiComboBox { model: ["A", "B", "C"]; width: 180; enabled: false }
            }
            Item { Layout.fillWidth: true }
        }
    }

    // ============ 按钮 ============
    SettingsSection {
        Layout.fillWidth: true
        title: Strings.t("gallery.buttons")

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 4
            Layout.bottomMargin: 8
            spacing: 12

            UiButton { variant: "filled"; text: Strings.t("gallery.btn-filled") }
            UiButton { text: Strings.t("gallery.btn-outlined") }
            UiButton { text: Strings.t("gallery.btn-danger"); danger: true }
            UiButton { text: Strings.t("gallery.btn-disabled"); enabled: false }
            Item { Layout.fillWidth: true }
        }
    }

    // 验收辅助：首次进入本页 800ms 后自动展开示例下拉（closePolicy 保留默认，Escape/外点可关）。
    Timer {
        interval: 800
        repeat: false
        running: gallery.visible && !gallery.autoDemoShown
        onTriggered: {
            gallery.autoDemoShown = true;
            demoCombo.popup.open();
        }
    }
}
