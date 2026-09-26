// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "../components"

// 登录态页（参考截图代理组页的列表结构）：
//   概览行（总数/域名数 + 危险操作，双确认）+ 全宽折叠域名行 + 插件配对指引卡。
// Cookie 值永不显示；列表来自 CookiesModel（domain/count 角色）。
ColumnLayout {
    id: page
    spacing: Theme.gap

    Component.onCompleted: Cookies.refresh()

    property string copyToast: ""

    // ============ 概览 + 操作 ============
    UiCard {
        Layout.fillWidth: true
        title: Strings.t("cookies.title")
        glyph: "lock"

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            UiText {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: Strings.t("cookies.total") + " " + Cookies.total
                      + "    ·    " + Strings.t("cookies.domains") + " " + Cookies.domainCount
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            UiButton { text: Strings.t("cookies.flush"); onClicked: Cookies.flushStore() }
            UiButton {
                text: Strings.t("cookies.clear-session")
                onClicked: clearConfirm.open()
            }
            UiButton {
                variant: "filled"
                danger: true
                text: Strings.t("cookies.purge")
                enabled: Cookies.total > 0
                onClicked: purgeConfirm.open()
            }
        }
    }

    // ============ 域名折叠列表 ============
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: Cookies.domainCount > 0

        Repeater {
            model: Cookies

            AccordionCard {
                Layout.fillWidth: true
                required property string domain
                required property int count
                title: domain
                badge: count + " " + Strings.t("cookies.count")
                subtitle: Strings.t("cookies.via-sync")
                glyph: "globe"

                UiText {
                    Layout.fillWidth: true
                    text: Strings.t("cookies.row-detail")
                    color: Theme.secondaryText
                    font.pixelSize: Theme.captionSize
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                }
            }
        }
    }
    UiCard {
        Layout.fillWidth: true
        visible: Cookies.domainCount === 0
        title: Strings.t("cookies.empty")
        glyph: "globe"
        UiText {
            Layout.fillWidth: true
            text: Strings.t("cookies.empty-desc")
            color: Theme.secondaryText
            wrapMode: Text.WordWrap
            maximumLineCount: 3
        }
    }

    // ============ 插件配对指引 ============
    UiCard {
        Layout.fillWidth: true
        title: Strings.t("cookies.pairing")
        glyph: "link"

        UiText {
            Layout.fillWidth: true
            text: Strings.t("cookies.pairing-desc")
            color: Theme.secondaryText
            wrapMode: Text.WordWrap
            maximumLineCount: 3
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredHeight: 34
                radius: 6
                color: Theme.inset
                border.width: 1
                border.color: Theme.divider
                UiText {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                    font.pixelSize: 12
                    text: ServiceInfo.adminUrl + "cookies"
                }
            }
            UiButton {
                text: Strings.t("common.copy")
                onClicked: {
                    ServiceInfo.copyText(ServiceInfo.adminUrl + "cookies");
                    page.copyToast = Strings.t("common.copied");
                    toast.show();
                }
            }
            UiButton {
                text: Strings.t("settings.token")
                onClicked: {
                    ServiceInfo.copyText(ServiceInfo.accessToken());
                    page.copyToast = Strings.t("common.copied");
                    toast.show();
                }
            }
        }
    }

    Toast { id: toast; text: page.copyToast }

    ConfirmPopup {
        id: clearConfirm
        title: Strings.t("cookies.clear-session")
        message: Strings.t("cookies.clear-session-desc")
        confirmText: Strings.t("common.confirm")
        onAccepted: Cookies.clearSession()
    }
    ConfirmPopup {
        id: purgeConfirm
        danger: true
        title: Strings.t("cookies.purge")
        message: Strings.t("cookies.purge-desc") + "\n\n" + Strings.t("cookies.purge-confirm")
        confirmText: Strings.t("cookies.purge")
        onAccepted: Cookies.purgePermanent()
    }
}
