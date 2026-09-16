// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QString>
#include <QUrl>

namespace seimi {

// 登录墙启发式（保守，仅 URL 特征）：final_url 命中登录特征即判定。
// 渲染管线（RenderWorker collect）与 /fetch 直取（HttpServer）共用同一份判定，
// 防止两条管线判定漂移（backlog #10：fetch 侧曾缺失此检测，登录重定向伪装 200）。
// 刻意不做内容特征（title/body 含 "Log in"）：误报会把正常内容页标成登录墙，代价高于
// 漏报；漏报的兜底是 final_url 本身已透出，调用方可自行判断。
// 命中特征放进 reason 供复核。
inline void detectAuthWall(const QString& finalUrl, bool* hit, QString* reason) {
    *hit = false;
    reason->clear();
    if (finalUrl.isEmpty()) return;
    const QUrl u(finalUrl);
    const QString path = u.path().toLower();
    for (const QString& seg : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        // contains 类：login.jsp / wp-login.php / signin / sign-in / logon 等变体。
        if (seg.contains(QLatin1String("login")) || seg.contains(QLatin1String("signin"))
            || seg.contains(QLatin1String("sign-in")) || seg.contains(QLatin1String("logon"))) {
            *hit = true;
            *reason = QStringLiteral("path segment '%1'").arg(seg);
            return;
        }
        // 精确类：sso / oauth / cas——SSO 网关的标志性一级路径段。
        if (seg == QLatin1String("sso") || seg == QLatin1String("oauth")
            || seg == QLatin1String("cas")) {
            *hit = true;
            *reason = QStringLiteral("path segment '%1'").arg(seg);
            return;
        }
        // 前缀类：auth / authenticate / authorize（注意排除 /author/ 博客作者页）。
        if (seg == QLatin1String("auth") || seg.startsWith(QLatin1String("authenticat"))
            || seg.startsWith(QLatin1String("authorize")) || seg.startsWith(QLatin1String("authoris"))) {
            *hit = true;
            *reason = QStringLiteral("path segment '%1'").arg(seg);
            return;
        }
    }
    // Jira 特征指纹（2026-09-16 真实会话实撞）：会话/权限校验失败 302 到
    // login.jsp?permissionViolation=true。permissionViolation 也可能出现在非 login
    // 路径的 URL 上，单独按 query 指纹判一次。
    if (u.query(QUrl::FullyEncoded).contains(QLatin1String("permissionViolation"),
                                             Qt::CaseInsensitive)) {
        *hit = true;
        *reason = QStringLiteral("query 'permissionViolation' (authenticated but path check rejected)");
    }
}

} // namespace seimi
