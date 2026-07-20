// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Google 应用层反爬拦截页检测（header-only 纯函数）。
// Google 触发人机验证时重定向到 /sorry/index（或以 /sorry 路径直接应答）：
//   https://www.google.com/sorry/index?q=...&continue=...
// 判定 = host 属 google 域（含子域与各 ccTLD：www.google.com / google.com.hk /
// www.google.co.jp ...）且 path 以 /sorry 开头。
// 已知取舍：google.evil.com 这类伪造子域理论上误命中，但攻击者无动机构造
//（代价仅多两次重试后按 blocked 失败），不值得引入 eTLD+1 复杂度。

#include <QRegularExpression>
#include <QUrl>

namespace seimi {

inline bool isGoogleSorryUrl(const QUrl& url) {
    static const QRegularExpression re(QStringLiteral("(^|\\.)google\\.[a-z.]+$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(url.host()).hasMatch()
        && url.path().startsWith(QLatin1String("/sorry"));
}

} // namespace seimi
