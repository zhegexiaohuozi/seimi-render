<!-- SPDX-FileCopyrightText: 2026 wanghaomiao.cn
 SPDX-License-Identifier: Apache-2.0
-->
# seimi-render Cookie 同步插件

一键把 Chrome 的登录态（cookies）同步到本地的 [seimi-render](../README.md) 渲染服务，让渲染带上你的登录状态——这样 seimi-render 就能渲染需要登录的页面（你已登录的后台、付费内容、个人主页等）。

## 它解决什么

seimi-render 默认用干净的 WebEngine profile 渲染，没有你的登录态。需要渲染「登录后才看得见」的页面时，先用这个插件把 Chrome 里的 cookie 同步过去，渲染请求就会自动带上你的登录态。

## 安全模型

- **cookie 仅存内存**：同步到 seimi-render 的 cookie 只在进程内存里（`NoPersistentCookies`），不落盘；服务重启即清空，需重新同步。
- **本地传输**：默认端点 `http://localhost:8088`，cookie 不出本机。
- **插件不读取/不存储 cookie 明文以外的信息**，概览接口（`GET /cookies`）只返回「域名→数量」，不含 cookie value。
- **隐私可控**：默认全选，但你可以勾选只同步需要的域名（不要把银行/邮箱等敏感会话也灌进去）。

## 安装（开发者模式加载）

1. 启动 seimi-render 服务（默认 `http://localhost:8088`）。
2. 打开 Chrome → 地址栏输入 `chrome://extensions`。
3. 右上角打开「**开发者模式**」。
4. 点「**加载已解压的扩展程序**」，选择本目录（`chrome-extension/`）。
5. 工具栏出现 seimi-render 图标，点开即可使用。

> 需要用的浏览器：基于 Chromium 的（Chrome / Edge / Brave 等都支持 MV3）。Firefox 的 API 名字不同，需自行适配。

## 使用流程

1. 点扩展图标，弹窗自动读取浏览器**所有** cookie，按域名聚合。
2. 顶部确认/修改 seimi-render 端点（默认 `http://localhost:8088`，会记忆）。
3. 顶部「全选」切换 / 搜索框过滤 / 逐个勾选要同步的域名（勾选状态会记忆，下次打开沿用）。
4. 点「**一键同步**」→ 显示「已同步 N cookies（服务端共 M）」，N/M 对账确认。
5. 之后 seimi-render 渲染这些域名的页面时自动带上登录态。
6. （可选）开启「**自动同步**」并设定周期（默认 30 秒）：后台按周期自动把勾选域名的 cookie 同步过去，弹窗关闭后依然生效；面板顶部显示最近一次自动同步的时间与结果。

## 功能要点

- **全选 / 全不选**：顶部一键切换。
- **搜索过滤**：站点多时快速定位（几百个域名也能流畅过滤）。
- **域名按 cookie 数倒序**：登录态重的站排最前。
- **连接状态徽标**：实时显示 seimi-render 是否连得上（「已连接」/「未连接」/「检测中」）。
- **对账**：同步后自动拉 `GET /cookies` 显示服务端实际持有的 cookie 总数，确认成功。
- **清空**：一键清空 seimi-render 上已同步的 cookie（换账号/调试时用）。
- **定时自动同步（默认关闭）**：面板顶部「自动同步」开关 + 周期输入（默认 30 秒，可自定义 30 秒–24 小时）。由 MV3 service worker + `chrome.alarms` 实现，**popup 关闭后依然生效**；只同步面板里勾选的域名（取消勾选的域名会被记住并排除，后台不会把敏感站灌进去）；面板实时显示最近一次自动同步的时间与结果（popup 开着时后台触发也会即时刷新）。
- **勾选记忆**：手动取消勾选的域名跨会话记忆，手动「一键同步」与后台自动同步共用同一份选择。
- **端点记忆**：上次输入的端点存 `chrome.storage`，下次自动填。
- **中英文切换**：界面默认跟随 Chrome 界面语言（中文 → 中文界面，其余 → 英文）；右上角按钮可一键切换，选择会记忆。

## 对应的后端接口

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/cookies` | 批量同步 cookies（插件调用） |
| GET | `/cookies` | 概览：域名→数量（对账用，不含 value） |
| DELETE | `/cookies` | 清空已同步的 cookie |

字段映射（Chrome `cookies.getAll()` → seimi-render）：

| Chrome 字段 | seimi-render 字段 | 说明 |
|-------------|-------------------|------|
| `name` / `value` | `name` / `value` | 必填 |
| `domain` | `domain` | host-only 时用作 origin host |
| `hostOnly` | `hostOnly` | true→精确 host 匹配，不含子域 |
| `secure` / `httpOnly` | `secure` / `httpOnly` | 直接映射 |
| `path` | `path` | 默认 `/` |
| `expirationDate` | `expirationDate` | epoch 秒；session cookie（≤0）当会话级 |

## 目录结构

```
chrome-extension/
├── manifest.json    # MV3 清单（name/description 走 __MSG_*__ 本地化）
├── background.js    # service worker：chrome.alarms 定时自动同步（默认关闭，开启前零后台活动）
├── sync-core.js     # 共享核心：收集 cookie 组装 POST /cookies payload（popup 与 background 共用，字段映射只有一份）
├── popup.html       # 面板结构（data-i18n* 标记可翻译文案）
├── popup.css        # 样式
├── popup.js         # 逻辑：读 cookie / 勾选 / 同步 / 对账 / 自动同步设置与状态展示 / 语言切换
├── i18n.js          # 国际化：中英文案表 + 检测/切换/应用
├── _locales/
│   ├── en/messages.json    # 英文（manifest name/description）
│   └── zh_CN/messages.json # 中文（manifest name/description）
├── icons/
│   └── icon.svg     # 图标源（MV3 default_icon 支持 SVG）
└── README.md        # 本文件
```

## 国际化（中/英）

- **默认语言**：跟随 Chrome 界面语言——`chrome.i18n.getUILanguage()` 返回 `zh-*` → 中文，其余 → 英文。
- **一键切换**：弹窗右上角按钮（中文界面显示「English」，英文界面显示「中文」），点击即切换，选择记忆到 `chrome.storage`，下次打开沿用。
- **manifest 本地化**：扩展名/描述通过 `_locales/` 提供，Chrome 扩展管理页会按界面语言显示。
- **动态文案**：状态提示、连接徽标、统计文案等运行时文本也随语言切换实时更新。

## 注意

- **SameSite**：Qt 的 `QNetworkCookie` 在 Qt 6.7 对 SameSite 的支持有限；部分标了 `SameSite=None; Secure` 的跨站 cookie 可能行为与浏览器略有差异，但绝大多数登录态 cookie（host-only / `SameSite=Lax`）能正常生效。
- **HttpOnly cookie 也能同步**：插件用 `chrome.cookies` API 读，绕过 JS 限制，HttpOnly cookie 照样能读出并同步（这正是关键——登录 cookie 多为 HttpOnly）。
- **自动同步的最小周期是 30 秒**：受 `chrome.alarms` 限制（Chrome 120+ 支持 30s；更老版本会被浏览器静默钳到 1 分钟）。默认关闭，开启前没有任何后台活动；开关与周期改完落库即生效（后台监听存储变化重排定时器）。
- **自动同步随浏览器运行**：浏览器关闭自然停止，重新打开后自动恢复；后台同步失败（seimi-render 未启动 / token 错误等）不会自动关闭开关，会在面板显示最近一次的错误，服务恢复后自动继续。
