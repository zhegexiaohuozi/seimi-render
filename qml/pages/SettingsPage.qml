// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "../components"

// 设置页（精修版）：SettingsSection 分组（无大图标头/无贯穿分隔线）+ SettingRow
//（标签可伸缩省略、控件限宽右对齐、不 fillWidth）。宽窗双列分组，窄窗单列。
// 控件宽度规格：端口 110 / 地址 220 / 数字 132 / 开关 42 / 下拉 160。
// 保存语义不变：点保存 → Settings.save() 写 config.json（代理可选立即生效）。
ColumnLayout {
    id: page
    spacing: Theme.gap

    readonly property var ev: Settings.effective
    readonly property var cli: Settings.cliFlags

    function cliBadge(key) {
        return cli && cli[key] === true ? Strings.t("settings.badge.cli") : "";
    }
    function cliBlocked(key) {
        return cli && cli[key] === true;
    }

    GridLayout {
        Layout.fillWidth: true
        columns: width >= 900 ? 2 : 1
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap

        // —— 网络服务 ——
        SettingsSection {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("settings.network")
            badgeText: Strings.t("settings.badge.restart")

            SettingRow {
                label: Strings.t("settings.host")
                cliBadgeText: page.cliBadge("host")
                warning: fHost.text.trim() === "0.0.0.0" ? Strings.t("settings.expose-warn") : ""
                field: UiTextField {
                    id: fHost
                    width: 220
                    enabled: !page.cliBlocked("host")
                    text: page.ev.host ?? "127.0.0.1"
                    placeholderText: "127.0.0.1"
                    errorState: text.trim() === "0.0.0.0"
                }
                hint: Strings.t("settings.host-hint")
            }
            SettingRow {
                label: Strings.t("settings.http-port")
                cliBadgeText: page.cliBadge("httpPort")
                field: UiTextField { id: fHttpPort; width: 110; enabled: !page.cliBlocked("httpPort"); text: String(page.ev.httpPort ?? 8088);
                    validator: IntValidator { bottom: 1; top: 65535 } }
            }
            SettingRow {
                label: Strings.t("settings.ws-port")
                cliBadgeText: page.cliBadge("wsPort")
                field: UiTextField { id: fWsPort; width: 110; enabled: !page.cliBlocked("wsPort"); text: String(page.ev.wsPort ?? 8089);
                    validator: IntValidator { bottom: 1; top: 65535 } }
            }
            SettingRow {
                label: Strings.t("settings.mcp-port")
                cliBadgeText: page.cliBadge("mcpPort")
                field: UiTextField { id: fMcpPort; width: 110; enabled: !page.cliBlocked("mcpPort"); text: String(page.ev.mcpPort ?? 8090);
                    validator: IntValidator { bottom: 1; top: 65535 } }
            }
        }

        // —— 渲染性能 ——
        SettingsSection {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("settings.performance")
            badgeText: Strings.t("settings.badge.restart")

            SettingRow {
                label: Strings.t("settings.concurrency")
                cliBadgeText: page.cliBadge("concurrency")
                field: UiSpinBox { id: fConcurrency; from: 1; to: 64; value: page.ev.concurrency ?? 3;
                    editable: true; enabled: !page.cliBlocked("concurrency") }
            }
            SettingRow {
                label: Strings.t("settings.http-threads")
                cliBadgeText: page.cliBadge("httpThreads")
                field: UiSpinBox { id: fThreads; from: 2; to: 128; value: page.ev.httpThreads ?? 8;
                    editable: true; enabled: !page.cliBlocked("httpThreads") }
            }
            SettingRow {
                label: Strings.t("settings.settle-ms")
                cliBadgeText: page.cliBadge("settleMs")
                field: UiSpinBox { id: fSettle; from: 0; to: 600000; stepSize: 250; value: Math.min(600000, page.ev.settleMs ?? 2000);
                    editable: true; enabled: !page.cliBlocked("settleMs") }
            }
            SettingRow {
                label: Strings.t("settings.load-timeout-ms")
                cliBadgeText: page.cliBadge("loadTimeoutMs")
                field: UiSpinBox { id: fTimeout; from: 1000; to: 600000; stepSize: 1000; value: Math.max(1000, Math.min(600000, page.ev.loadTimeoutMs ?? 30000));
                    editable: true; enabled: !page.cliBlocked("loadTimeoutMs") }
            }
        }

        // —— 安全 ——
        SettingsSection {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("settings.security")
            badgeText: Strings.t("settings.badge.restart")

            SettingRow {
                label: Strings.t("settings.password")
                field: UiTextField { id: fPassword; width: 220; echoMode: TextInput.Password; text: page.ev.password ?? "" }
                hint: Strings.t("settings.password-note")
            }
            SettingRow {
                label: Strings.t("settings.ssrf")
                cliBadgeText: page.cliBadge("ssrfGuard")
                field: UiSwitch { id: fSsrf; checked: page.ev.ssrfGuard ?? true; enabled: !page.cliBlocked("ssrfGuard") }
                warning: !fSsrf.checked ? Strings.t("settings.ssrf-warn") : ""
            }
            SettingRow {
                label: Strings.t("settings.stealth")
                cliBadgeText: page.cliBadge("stealth")
                field: UiSwitch { id: fStealth; checked: page.ev.stealth ?? true; enabled: !page.cliBlocked("stealth") }
            }
            SettingRow {
                label: Strings.t("settings.admin-ui")
                cliBadgeText: page.cliBadge("adminEnabled")
                field: UiSwitch { id: fAdmin; checked: page.ev.adminEnabled ?? true; enabled: !page.cliBlocked("adminEnabled") }
            }
            SettingRow {
                label: Strings.t("settings.trusted-proxy")
                cliBadgeText: page.cliBadge("trustedProxies")
                field: UiTextField { id: fTrusted; width: 220; enabled: !page.cliBlocked("trustedProxies");
                    text: page.ev.trustedProxies ?? ""; placeholderText: "10.8.0.0/16,127.0.0.1" }
            }
        }

        // —— 数据 ——
        SettingsSection {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("settings.data")
            badgeText: Strings.t("settings.badge.restart")

            SettingRow {
                label: Strings.t("settings.data-dir")
                cliBadgeText: page.cliBadge("dataDir")
                field: RowLayout {
                    spacing: 6
                    UiTextField {
                        id: fDataDir
                        width: 220
                        enabled: !page.cliBlocked("dataDir")
                        text: page.ev.dataDir ?? ""
                        placeholderText: ServiceInfo.dataDir
                    }
                    UiButton {
                        text: Strings.t("settings.browse")
                        enabled: !page.cliBlocked("dataDir")
                        onClicked: dirPicker.open()
                    }
                }
                hint: Strings.t("settings.data-dir-hint")
            }
            SettingRow {
                label: ""
                field: UiButton { text: Strings.t("settings.open-dir"); onClicked: ServiceInfo.openDataDir() }
                hint: Strings.t("settings.data-dir-current") + ServiceInfo.dataDir
            }
            SettingRow {
                label: Strings.t("settings.warmup")
                cliBadgeText: page.cliBadge("warmup")
                field: UiSwitch { id: fWarmup; checked: page.ev.warmup ?? true; enabled: !page.cliBlocked("warmup") }
            }
            SettingRow {
                label: Strings.t("settings.warmup-url")
                cliBadgeText: page.cliBadge("warmupUrl")
                field: UiTextField { id: fWarmupUrl; width: 220; enabled: !page.cliBlocked("warmupUrl"); text: page.ev.warmupUrl ?? "" }
            }
            SettingRow {
                label: Strings.t("settings.remote-debug")
                cliBadgeText: page.cliBadge("remoteDebugPort")
                field: UiTextField { id: fCdp; width: 110; enabled: !page.cliBlocked("remoteDebugPort"); text: String(page.ev.remoteDebugPort ?? 0);
                    validator: IntValidator { bottom: 0; top: 65535 } }
                hint: Strings.t("settings.remote-debug-note")
            }
        }

        // —— 网络代理（热生效；整行跨两列时不拆，放第三行单独占位） ——
        SettingsSection {
            Layout.fillWidth: true
            Layout.columnSpan: page.width >= 900 ? 2 : 1
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("settings.proxy")
            badgeText: Strings.t("settings.badge.now")

            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.bottomMargin: 8
                columns: width >= 560 ? 2 : 1
                columnSpacing: 24
                rowSpacing: 0

                // 代理组内改用两栏 SettingRow 网格：代理参数多，双列省纵向空间。
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.minimumWidth: 0
                    spacing: 0
                    SettingRow {
                        label: Strings.t("settings.proxy-type")
                        cliBadgeText: page.cliBadge("proxy")
                        field: UiComboBox {
                            id: fProxyType
                            width: 160
                            model: [Strings.t("settings.direct"), "HTTP", "SOCKS5"]
                            currentIndex: page.ev.proxyType === "http" ? 1
                                : page.ev.proxyType === "socks5" ? 2 : 0
                        }
                    }
                    SettingRow {
                        label: Strings.t("settings.proxy-host")
                        field: UiTextField { id: fProxyHost; width: 220; text: page.ev.proxyHost ?? ""; placeholderText: "127.0.0.1" }
                    }
                    SettingRow {
                        label: Strings.t("settings.proxy-port")
                        field: UiTextField { id: fProxyPort; width: 110; text: String(page.ev.proxyPort ?? 0);
                            validator: IntValidator { bottom: 0; top: 65535 } }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.minimumWidth: 0
                    spacing: 0
                    SettingRow {
                        label: Strings.t("settings.proxy-user")
                        field: UiTextField { id: fProxyUser; width: 220; text: page.ev.proxyUser ?? "" }
                    }
                    SettingRow {
                        label: Strings.t("settings.proxy-pass")
                        field: UiTextField { id: fProxyPass; width: 220; text: page.ev.proxyPass ?? ""; echoMode: TextInput.Password }
                    }
                    SettingRow {
                        label: Strings.t("settings.proxy-apply-now")
                        field: UiCheckBox { id: fProxyApplyNow; checked: true }
                    }
                }
            }
            SettingRow {
                Layout.fillWidth: true
                label: Strings.t("settings.bypass")
                field: UiText {
                    width: 240
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideMiddle
                    text: (page.ev.proxyBypass ?? "") === "" ? "—" : page.ev.proxyBypass
                    color: Theme.secondaryText
                    font.pixelSize: Theme.captionSize
                }
                warning: (page.ev.proxyBypass ?? "") !== "" ? Strings.t("settings.bypass-note") : ""
            }
        }
    }

    // —— 保存 ——
    RowLayout {
        Layout.fillWidth: true
        Layout.rightMargin: 12
        Layout.bottomMargin: 8
        Item { Layout.fillWidth: true }
        UiButton {
            variant: "filled"
            text: Strings.t("common.save")
            onClicked: {
                const proxyType = fProxyType.currentIndex === 1 ? "http"
                    : fProxyType.currentIndex === 2 ? "socks5" : "direct";
                const form = {
                    host: fHost.text.trim(),
                    httpPort: parseInt(fHttpPort.text) || 8088,
                    wsPort: parseInt(fWsPort.text) || 8089,
                    mcpPort: parseInt(fMcpPort.text) || 8090,
                    concurrency: fConcurrency.value,
                    httpThreads: fThreads.value,
                    settleMs: fSettle.value,
                    loadTimeoutMs: fTimeout.value,
                    password: fPassword.text,
                    adminEnabled: fAdmin.checked,
                    ssrfGuard: fSsrf.checked,
                    stealth: fStealth.checked,
                    warmup: fWarmup.checked,
                    warmupUrl: fWarmupUrl.text.trim(),
                    remoteDebugPort: parseInt(fCdp.text) || 0,
                    trustedProxies: fTrusted.text.trim(),
                    proxyType: proxyType,
                    proxyHost: fProxyHost.text.trim(),
                    proxyPort: parseInt(fProxyPort.text) || 0,
                    proxyUser: fProxyUser.text,
                    proxyPass: fProxyPass.text,
                    proxyBypass: page.ev.proxyBypass ?? "",
                    dataDir: fDataDir.text.trim(),
                };
                const r = Settings.save(form, fProxyApplyNow.checked);
                saveResult.ok = r.ok;
                saveResult.message = !r.ok ? Strings.t("settings.save-fail")
                    + (r.error !== undefined ? "\n" + r.error : "")
                    : (r.needsRestart ? Strings.t("settings.saved-restart")
                                      : Strings.t("settings.saved"));
                saveResult.show();
            }
        }
    }

    Toast {
        id: saveResult
        property bool ok: true
        property string message: ""
        durationMs: 2400
        text: saveResult.message
    }

    // 数据目录选择（系统文件夹对话框；file URL 转本地路径）。
    FolderDialog {
        id: dirPicker
        title: Strings.t("settings.data-dir")
        onAccepted: {
            const p = decodeURIComponent(selectedFolder.toString())
                .replace(/^file:\/\/\//, "").replace(/^file:\/\//, "");
            fDataDir.text = p;
        }
    }
}
