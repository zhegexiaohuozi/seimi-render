// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "../components"

// 更新页：版本信息 + 更新源配置 + 检查/下载/校验/应用状态机面板。
ColumnLayout {
    id: page
    spacing: Theme.gap

    readonly property string st: Updater.state

    function stateTone(s) {
        return s === "ready" ? Theme.success
            : s === "error" ? Theme.danger
            : s === "available" ? Theme.primary
            : (s === "downloading" || s === "checking" || s === "verifying" || s === "applying")
                ? Theme.warning : Theme.secondaryText;
    }

    // —— 当前版本 ——
    UiCard {
        Layout.fillWidth: true
        title: Strings.t("update.current")
        glyph: "update"

        GridLayout {
            Layout.fillWidth: true
            columns: width < 620 ? 1 : 2
            columnSpacing: 16
            rowSpacing: 6

            UiText {
                text: "v" + Updater.currentVersion
                color: Theme.primary
                font.pixelSize: Theme.titleSize
                font.weight: Font.DemiBold
            }
            RowLayout {
                spacing: 8
                StatusBadge {
                    text: Updater.installerMode
                        ? Strings.t("update.mode-installer")
                        : Strings.t("update.mode-portable")
                }
                StatusBadge { text: ServiceInfo.platformKey }
            }
            UiText {
                text: Strings.t("update.build-time") + ": " + Updater.buildTime
                color: Theme.secondaryText
                font.pixelSize: Theme.captionSize
                elide: Text.ElideMiddle
            }
            UiText {
                text: Strings.t("update.git") + ": " + Updater.gitCommit
                color: Theme.secondaryText
                font.pixelSize: Theme.captionSize
            }
        }
    }

    // —— 更新源（统一表单：SettingsSection + SettingRow，控件族/宽度规格与设置页一致）——
    SettingsSection {
        Layout.fillWidth: true
        title: Strings.t("update.source")

        SettingRow {
            label: Strings.t("update.manifest")
            field: UiTextField {
                id: manifestField
                width: 220
                text: Updater.manifestUrl
                placeholderText: Strings.t("update.manifest-ph")
                onEditingFinished: Updater.manifestUrl = text.trim()
            }
            warning: Updater.manifestUrl === "" ? Strings.t("update.no-url") : ""
        }
        SettingRow {
            label: Strings.t("update.channel")
            field: UiComboBox {
                width: 160
                model: [Strings.t("update.channel-stable"), Strings.t("update.channel-beta")]
                currentIndex: GuiPrefs.updateChannel === "beta" ? 1 : 0
                onActivated: function(i) {
                    GuiPrefs.updateChannel = i === 1 ? "beta" : "stable";
                }
            }
        }
        SettingRow {
            label: Strings.t("update.auto")
            field: UiSwitch {
                checked: GuiPrefs.autoCheckUpdates
                onToggled: GuiPrefs.autoCheckUpdates = checked
            }
        }
    }

    // —— 状态机面板 ——
    UiCard {
        Layout.fillWidth: true
        title: Strings.t("nav.update")
        glyph: "update"
        badgeText: page.st
        badgeTone: page.stateTone(page.st)

        // idle / error：检查按钮。
        RowLayout {
            visible: page.st === "idle" || page.st === "error"
                || page.st === "up-to-date" || page.st === "available"
            spacing: 12
            UiButton {
                variant: "filled"
                text: Strings.t("update.check")
                enabled: !Updater.busy
                onClicked: Updater.checkNow()
            }
            RowLayout {
                visible: page.st === "up-to-date"
                spacing: 6
                UiIcon { glyph: "check"; width: 14; height: 14; strokeWidth: 2.4; ink: Theme.success }
                UiText { text: Strings.t("update.up-to-date"); color: Theme.success }
            }
            UiText {
                visible: page.st === "error"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: Strings.t("update.error") + ": " + Updater.errorMessage
                color: Theme.danger
                font.pixelSize: Theme.captionSize
                wrapMode: Text.WordWrap
                maximumLineCount: 3
            }
        }

        // available：版本 + 说明 + 下载按钮。
        ColumnLayout {
            visible: page.st === "available" || page.st === "error" && Updater.availableVersion !== ""
            spacing: 8
            Layout.fillWidth: true
            RowLayout {
                spacing: 10
                UiText {
                    text: Strings.t("update.available") + "：v" + Updater.availableVersion
                    color: Theme.primary
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                StatusBadge {
                    visible: Updater.packageSizeText !== "—"
                    text: Updater.packageSizeText
                }
                Item { Layout.fillWidth: true }
                UiButton {
                    variant: "filled"
                    visible: page.st === "available"
                    text: Strings.t("update.download")
                    onClicked: Updater.startDownload()
                }
                UiButton {
                    visible: page.st === "error"
                    text: Strings.t("common.retry")
                    onClicked: Updater.startDownload()
                }
            }
            NotesBox {}
        }

        // checking / verifying：进行中指示。
        RowLayout {
            visible: page.st === "checking" || page.st === "verifying"
            spacing: 10
            BusyIndicator { Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
            UiText {
                text: page.st === "checking"
                    ? Strings.t("update.checking")
                    : Strings.t("update.verifying")
                color: Theme.secondaryText
            }
        }

        // downloading：进度条 + 取消。
        ColumnLayout {
            visible: page.st === "downloading"
            spacing: 8
            Layout.fillWidth: true
            RowLayout {
                Layout.fillWidth: true
                UiText { text: Strings.t("update.downloading") + " · " + Updater.progressText }
                Item { Layout.fillWidth: true }
                UiButton { text: Strings.t("update.cancel"); onClicked: Updater.cancelDownload() }
            }
            RatioBar {
                Layout.fillWidth: true
                from: 0; to: 100
                value: Updater.progress
            }
        }

        // ready：重启更新。
        ColumnLayout {
            visible: page.st === "ready"
            spacing: 8
            Layout.fillWidth: true
            RowLayout {
                spacing: 6
                UiIcon { glyph: "check"; width: 14; height: 14; strokeWidth: 2.4; ink: Theme.success }
                UiText {
                    text: Strings.t("update.ready") + "（v" + Updater.availableVersion + "）"
                    color: Theme.success
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
            }
            UiButton {
                variant: "filled"
                text: Strings.t("update.restart")
                onClicked: Updater.applyAndRestart()
            }
        }

        // applying。
        RowLayout {
            visible: page.st === "applying"
            spacing: 10
            BusyIndicator { Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
            UiText { text: Strings.t("update.applying"); color: Theme.secondaryText }
        }
    }

    // —— 项目与反馈：GitHub 仓库 / Issue 引导（默认浏览器打开）——
    SettingsSection {
        id: feedbackSection
        Layout.fillWidth: true
        title: Strings.t("update.feedback")

        readonly property string repoUrl: "https://github.com/zhegexiaohuozi/seimi-render"
        readonly property string issuesUrl: repoUrl + "/issues"

        SettingRow {
            label: Strings.t("update.repo")
            field: RowLayout {
                spacing: 6
                UiText {
                    text: "github.com/zhegexiaohuozi/seimi-render"
                    color: Theme.primary
                    elide: Text.ElideMiddle
                    Layout.minimumWidth: 60
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Qt.openUrlExternally(feedbackSection.repoUrl)
                    }
                }
                UiButton {
                    text: Strings.t("common.open")
                    onClicked: Qt.openUrlExternally(feedbackSection.repoUrl)
                }
            }
            hint: Strings.t("update.repo-hint")
        }
        SettingRow {
            label: Strings.t("update.issues")
            field: RowLayout {
                spacing: 6
                UiText {
                    text: "github.com/zhegexiaohuozi/seimi-render/issues"
                    color: Theme.primary
                    elide: Text.ElideMiddle
                    Layout.minimumWidth: 60
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Qt.openUrlExternally(feedbackSection.issuesUrl)
                    }
                }
                UiButton {
                    text: Strings.t("common.open")
                    onClicked: Qt.openUrlExternally(feedbackSection.issuesUrl)
                }
            }
            hint: Strings.t("update.issues-hint")
        }
    }

    Item { Layout.fillHeight: true; Layout.bottomMargin: 8 }

    component NotesBox: Item {
        Layout.fillWidth: true
        implicitHeight: Math.min(notesText.implicitHeight, 260) + 16
        visible: notesText.text !== ""
        Rectangle {
            anchors.fill: parent
            radius: Theme.cardRadius
            color: Theme.inset
            border.width: 1
            border.color: Theme.divider
            Flickable {
                id: notesFlick
                anchors.fill: parent
                anchors.margins: 8
                contentWidth: width
                contentHeight: notesText.implicitHeight
                clip: true
                ScrollBar.vertical: ScrollBar {}
                UiText {
                    id: notesText
                    width: notesFlick.width
                    text: GuiPrefs.locale === "zh" ? Updater.notesZh : Updater.notesEn
                    color: Theme.secondaryText
                    font.pixelSize: Theme.captionSize
                    wrapMode: Text.WordWrap
                    maximumLineCount: 1000
                }
            }
        }
    }
}
