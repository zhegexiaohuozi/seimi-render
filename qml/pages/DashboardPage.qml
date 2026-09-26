// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeimiRender.Gui 1.0
import ".."
import "../components"

// 概览页（参考截图首页节奏）：
//   第一行 渲染服务（整行，展示全部服务监听端口）；第二行 登录态 / MCP 服务；
//   第三行 整行渲染统计（曲线上方 + 紧凑指标格下方）；第四行 域名分布(左) / 运行环境(右，含网络代理状态)。
// 无数据字段显示 "—"；负载条只用真实分子/分母。外壳的 ScrollView 负责滚动。
ColumnLayout {
    id: page
    spacing: Theme.gap

    Component.onCompleted: Dashboard.refresh()

    readonly property bool hasSamples: Dashboard.totalTasks > 0

    // 运行环境信息行：定宽标签列 + 值列（可选换行/次级色）。文件级内联组件。
    component EnvRow: RowLayout {
        id: envRow
        property string rowLabel: ""
        property string rowValue: ""
        property bool secondary: false
        property bool wrap: false
        spacing: 10
        UiText {
            Layout.preferredWidth: 72
            text: envRow.rowLabel
            color: Theme.secondaryText
            font.pixelSize: Theme.captionSize
        }
        UiText {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: envRow.rowValue
            color: envRow.secondary ? Theme.secondaryText : Theme.text
            font.pixelSize: Theme.bodySize
            wrapMode: envRow.wrap ? Text.WordWrap : Text.NoWrap
            elide: envRow.wrap ? Text.ElideNone : Text.ElideMiddle
        }
    }

    // ============ 第一/二行：2×2 卡片 ============
    GridLayout {
        Layout.fillWidth: true
        columns: width >= 680 ? 2 : 1
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap

        // —— 渲染服务（整行：全部服务监听端口 + 负载）——
        UiCard {
            Layout.fillWidth: true
            Layout.columnSpan: 2
            Layout.minimumWidth: 0
            title: Strings.t("dashboard.service")
            glyph: "server"
            // 打开 admin-ui（默认浏览器）——与托盘「打开 Web 控制台」同款动作。
            actionText: Strings.t("dashboard.open-console")
            onActionTriggered: ServiceInfo.openAdminConsole()

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                UiText { Layout.fillWidth: true; Layout.minimumWidth: 0; text: Strings.t("dashboard.status"); color: Theme.secondaryText; font.pixelSize: Theme.captionSize }
                StatusBadge { text: Strings.t("footer.running"); tone: Theme.success; filled: true }
            }
            // 全部服务端口占用（HTTP / WebSocket / MCP），值 = 监听地址 host:port。
            GridLayout {
                Layout.fillWidth: true
                columns: width >= 620 ? 3 : 1
                columnSpacing: 16
                rowSpacing: 4
                EnvRow { rowLabel: Strings.t("dashboard.port-http"); rowValue: ServiceInfo.host + ":" + ServiceInfo.httpPort }
                EnvRow { rowLabel: Strings.t("dashboard.port-ws"); rowValue: ServiceInfo.host + ":" + ServiceInfo.wsPort }
                EnvRow { rowLabel: Strings.t("dashboard.port-mcp"); rowValue: ServiceInfo.host + ":" + ServiceInfo.mcpPort }
            }
            UiText {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: Strings.t("dashboard.workers") + "  " + Dashboard.workersBusy + " / " + Dashboard.workersTotal
                      + "    " + Strings.t("dashboard.pending") + "  " + Dashboard.queuePending
                color: Theme.secondaryText
            }
            RatioBar { Layout.fillWidth: true; value: Dashboard.workerUsage }
        }

        // —— 登录态 ——
        UiCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("dashboard.cookies")
            glyph: "lock"
            actionText: Strings.t("dashboard.manage")
            onActionTriggered: page.goToPage(4)

            UiText { Layout.fillWidth: true; text: Strings.t("dashboard.cookie-session"); font.pixelSize: 16 }
            UiText {
                Layout.fillWidth: true
                text: Dashboard.cookieDomains + " " + Strings.t("dashboard.domains-unit")
                      + "   ·   " + Dashboard.cookieTotal + " " + Strings.t("dashboard.cookies-unit")
                color: Theme.secondaryText
            }
            StatusBadge {
                text: Dashboard.cookieTotal > 0 ? Strings.t("dashboard.synced") : Strings.t("dashboard.waiting-sync")
                tone: Dashboard.cookieTotal > 0 ? Theme.success : Theme.warning
                filled: true
            }
        }

        // —— MCP 服务 ——
        UiCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            title: Strings.t("dashboard.mcp")
            glyph: "link"

            UiText { Layout.fillWidth: true; text: Strings.t("dashboard.mcp-desc"); font.pixelSize: 16 }
            UiText {
                Layout.fillWidth: true
                text: Strings.t("dashboard.mcp-sessions") + "  " + Dashboard.mcpSessions + " / " + Dashboard.mcpMax
                color: Theme.secondaryText
            }
            RatioBar { Layout.fillWidth: true; value: Dashboard.mcpUsage }
        }
    }

    // ============ 第三行：整行渲染统计 ============
    UiCard {
        Layout.fillWidth: true
        title: Strings.t("dashboard.stats")
        glyph: "chart"

        MiniChart {
            Layout.fillWidth: true
            samples: Throughput.completedSeries
            emptyText: Strings.t("dashboard.no-samples")
        }
        GridLayout {
            Layout.fillWidth: true
            columns: width >= 680 ? 4 : 2
            columnSpacing: 8; rowSpacing: 8
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: Strings.t("dashboard.kpi.success-rate"); glyph: "check"; tone: Theme.success; unit: "%"
                value: page.hasSamples ? Dashboard.successRateText : "—"
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: Strings.t("dashboard.kpi.throughput"); unit: "/s"
                value: page.hasSamples ? Dashboard.throughputText : "—"
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: "P50"; glyph: "clock"; tone: Theme.warning; unit: "ms"
                value: page.hasSamples ? Dashboard.p50Text : "—"
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: "P90"; glyph: "clock"; tone: Theme.warning; unit: "ms"
                value: page.hasSamples ? Dashboard.p90Text : "—"
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: Strings.t("dashboard.avg"); glyph: "clock"; tone: Theme.warning; unit: "ms"
                value: page.hasSamples ? Dashboard.avgText : "—"
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: Strings.t("dashboard.kpi.queue"); glyph: "server"
                value: String(Dashboard.queuePending)
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: Strings.t("dashboard.tasks"); glyph: "logs"; tone: Theme.success
                value: String(Dashboard.totalTasks)
            }
            MetricCell {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                label: Strings.t("dashboard.env-rss"); glyph: "cpu"; tone: Theme.danger
                value: Dashboard.rssText
            }
        }
    }

    // ============ 第四行：域名请求分布(左 2/3) / 运行环境(右 1/3) ============
    // 断点 880 逻辑像素（ GridLayout 自身宽度）：宽窗口双列，窄窗口上下排列。
    // 列宽用显式比例公式（Qt Quick Layouts 的 preferredWidth 是像素提示而非
    // stretch factor——实测 2/1 会被 implicitWidth 干扰成 ~3:1），preferred 之和
    // 恰好填满可用宽，fillWidth 分配后即精确 2:1 / 1:1（单列时）。
    GridLayout {
        id: bottomRow
        Layout.fillWidth: true
        columns: width >= 880 ? 2 : 1
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap

        // —— 域名请求分布：四列（域名/请求数/失败数/成功率）+ 相对请求量条 ——
        // （放在 GridLayout 首位 = 双列时居左；单列时在上。）
        UiCard {
            id: domainCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: (bottomRow.width - bottomRow.columnSpacing) * 2 / 3
            Layout.minimumWidth: 0
            title: Strings.t("dashboard.top-domains")
            subtitleText: Strings.t("dashboard.domains-subtitle")
            glyph: "globe"

            // 默认 Top 8；「展开全部」展示后端返回的全部（上限 100，见 DashboardModel）。
            readonly property int maxVisible: 8
            property bool expanded: false
            readonly property var rows: Dashboard.topDomains
            readonly property int visibleCount: expanded
                ? rows.length : Math.min(rows.length, maxVisible)
            // 量条归一化基准：当前榜单（已按请求数降序）首行即最大请求量。
            readonly property real maxRequests: rows.length > 0 ? Math.max(1, Number(rows[0].total)) : 1
            // 数字列定宽，表头与数据行共用，保证纵向对齐。
            readonly property int colRequests: 76
            readonly property int colFailed: 68
            readonly property int colRate: 84

            // 空状态：明确文案，不用假数据或 Cookie 域名数补齐。
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: domainCard.rows.length === 0
                Item { Layout.fillWidth: true; Layout.preferredHeight: 24 }
                UiText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: Strings.t("dashboard.empty-domains")
                    color: Theme.secondaryText
                }
                Item { Layout.fillWidth: true; Layout.preferredHeight: 24 }
            }

            // 表头（12px 次级色；数字列右对齐）。
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                visible: domainCard.rows.length > 0
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    UiText {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: Strings.t("dashboard.domain")
                        color: Theme.secondaryText; font.pixelSize: Theme.captionSize
                    }
                    UiText {
                        Layout.preferredWidth: domainCard.colRequests
                        text: Strings.t("dashboard.total")
                        color: Theme.secondaryText; font.pixelSize: Theme.captionSize
                        horizontalAlignment: Text.AlignRight
                    }
                    UiText {
                        Layout.preferredWidth: domainCard.colFailed
                        text: Strings.t("dashboard.failed-count")
                        color: Theme.secondaryText; font.pixelSize: Theme.captionSize
                        horizontalAlignment: Text.AlignRight
                    }
                    UiText {
                        Layout.preferredWidth: domainCard.colRate
                        text: Strings.t("dashboard.success-rate")
                        color: Theme.secondaryText; font.pixelSize: Theme.captionSize
                        horizontalAlignment: Text.AlignRight
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
            }

            // 数据行（40~44 高、正文 14；极淡分隔线 + 轻微悬停背景）。
            // 展开时行高参与页面统一滚动，卡片自身无第二套纵向滚动条。
            Repeater {
                model: domainCard.visibleCount
                Item {
                    id: domainRow
                    required property int index
                    readonly property var d: domainCard.rows[index]
                    Layout.fillWidth: true
                    implicitHeight: 42
                    // 悬停高亮 + 长域名全文提示共用一个行级 HoverHandler。
                    HoverHandler { id: rowHover }
                    ToolTip.visible: rowHover.hovered && domainLabel.truncated
                    ToolTip.delay: 400
                    ToolTip.text: domainLabel.text

                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: Theme.hover
                        visible: rowHover.hovered
                    }
                    RowLayout {
                        anchors.fill: parent
                        spacing: 12
                        UiText {
                            id: domainLabel
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: domainRow.d ? domainRow.d.domain : ""
                            font.pixelSize: Theme.bodySize
                            elide: Text.ElideMiddle
                        }
                        UiText {
                            Layout.preferredWidth: domainCard.colRequests
                            text: domainRow.d ? String(domainRow.d.total) : ""
                            font.pixelSize: Theme.bodySize
                            horizontalAlignment: Text.AlignRight
                        }
                        UiText {
                            Layout.preferredWidth: domainCard.colFailed
                            text: domainRow.d ? String(domainRow.d.failed) : ""
                            color: domainRow.d && domainRow.d.failed > 0 ? Theme.danger : Theme.secondaryText
                            font.pixelSize: Theme.bodySize
                            horizontalAlignment: Text.AlignRight
                        }
                        UiText {
                            Layout.preferredWidth: domainCard.colRate
                            text: page.rateText(domainRow.d)
                            color: Theme.secondaryText
                            font.pixelSize: Theme.bodySize
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                    // 相对请求量条（3px）：按当前榜单最大请求量归一化——是请求量对比，
                    // 不是成功率、也不代表全部请求占比。
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 2
                        height: 3
                        radius: 1.5
                        width: domainRow.d
                            ? Math.max(2, parent.width * (Number(domainRow.d.total) / domainCard.maxRequests))
                            : 0
                        color: Theme.alpha(Theme.primary, 0.30)
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.divider
                        visible: domainRow.index < domainCard.visibleCount - 1
                    }
                }
            }

            // 展开 / 收起（仅当后端记录数超过默认 Top 8 时出现）。
            RowLayout {
                Layout.fillWidth: true
                visible: domainCard.rows.length > domainCard.maxVisible
                Item { Layout.fillWidth: true }
                UiButton {
                    text: domainCard.expanded
                        ? Strings.t("common.collapse")
                        : Strings.t("common.expand-all") + " (" + domainCard.rows.length + ")"
                    onClicked: domainCard.expanded = !domainCard.expanded
                }
            }
        }

        // —— 运行环境：紧凑「标签 / 值」信息行（含网络代理状态；不人为拉大行距填满卡片）——
        // 代理配置入口随原「网络代理」卡片合并至此：右上「管理」跳设置页。
        UiCard {
            id: envCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: (bottomRow.width - bottomRow.columnSpacing) * 1 / 3
            Layout.minimumWidth: 0
            title: Strings.t("dashboard.env")
            glyph: "cpu"
            actionText: Strings.t("dashboard.manage")
            onActionTriggered: page.goToPage(1)

            // 标签列定宽对齐；值列占剩余宽度，系统名可换行、构建版本次级文字。
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                EnvRow { rowLabel: Strings.t("dashboard.env-os"); rowValue: Dashboard.envOs; wrap: true }
                EnvRow { rowLabel: Strings.t("dashboard.env-arch"); rowValue: Dashboard.envArch }
                EnvRow { rowLabel: Strings.t("dashboard.env-cpu"); rowValue: Dashboard.cpuText }
                EnvRow { rowLabel: Strings.t("dashboard.env-rss"); rowValue: Dashboard.rssText }
                EnvRow { rowLabel: Strings.t("dashboard.uptime"); rowValue: Dashboard.uptimeText }
                EnvRow {
                    rowLabel: Strings.t("dashboard.proxy")
                    rowValue: Dashboard.proxyActive ? Dashboard.proxySummary : Strings.t("dashboard.proxy-off")
                    secondary: !Dashboard.proxyActive
                }
                EnvRow {
                    rowLabel: Strings.t("dashboard.env-version")
                    rowValue: "v" + Updater.currentVersion + "  ·  " + Updater.gitCommit
                    secondary: true
                    wrap: true
                }
            }
        }
    }

    // 成功率：分母 = 该域终态任务数 total（无有效分母显示 "—"；百分比最多一位小数）。
    function rateText(d) {
        if (!d || !Number(d.total)) return "—";
        return (Number(d.succeeded) / Number(d.total) * 100).toFixed(1) + "%";
    }
    // 跳转到稳定页面 id（0=概览 1=设置 2=更新 3=日志 4=登录态；与 Main.pageNames 同序）。
    function goToPage(i) {
        { const w = Window.window; if (w) w.pageIndex = i; }
    }
}
