#pragma once

//
// SSRF 防护：拦截指向内网/链路本地/元数据地址的渲染目标。
//
// seimi-render 用真实 Chromium 加载用户提交 URL，攻击者可借此访问云元数据端点
//（169.254.169.254 窃取 IAM 凭证）、内网管理面板、本机回环服务，并以渲染后的
// HTML/PDF 回传。比普通 HTTP 客户端的 SSRF 更危险（Chromium 跟随重定向、执行 JS）。
//
// 判定两层：
//   1) host 是 IP 字面量：直接判网段。
//   2) host 是域名：QHostInfo 解析，对每个 A/AAAA 判网段（防 DNS rebinding）。
//      任一解析结果命中黑名单即拒绝。
//
// 仅依赖 Qt（QHostAddress / QHostInfo），header-only。
//

#include <QHostAddress>
#include <QHostInfo>
#include <QString>
#include <QUrl>

#include <string>

namespace seimi {

// 单个 IP 地址是否属于应拦截的网段。
// 覆盖：
//   IPv4: 0.0.0.0/8（"本机网络"）、10/8、172.16/12、192.168/16、
//         127/8（回环）、169.254/16（链路本地，含云元数据 169.254.169.254）、
//         100.64/10（CGNAT）、224/4（组播）、240/4（保留）、255.255.255.255（广播）。
//   IPv6: ::1（回环）、::（未指定）、fc00::/7（ULA 本地）、fe80::/10（链路本地）、
//         ff00::/8（组播）。IPv4-mapped（::ffff:a.b.c.d）会被 QHostAddress 视为 IPv4
//         并走 IPv4 分支，故同样覆盖。
inline bool isPrivateOrLoopbackAddress(const QHostAddress& addr) {
    if (addr.isNull()) return false;

    // IPv4
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v4 = addr.toIPv4Address();
        auto in = [&](quint32 base, int prefix) -> bool {
            const quint32 mask = prefix >= 32 ? 0xffffffffu
                                              : (~0u << (32 - prefix));
            return (v4 & mask) == (base & mask);
        };
        // 0.0.0.0/8
        if (in(0x00000000u, 8)) return true;
        // 10.0.0.0/8
        if (in(0x0A000000u, 8)) return true;
        // 172.16.0.0/12
        if (in(0xAC100000u, 12)) return true;
        // 192.168.0.0/16
        if (in(0xC0A80000u, 16)) return true;
        // 127.0.0.0/8（回环）
        if (in(0x7F000000u, 8)) return true;
        // 169.254.0.0/16（链路本地，含 169.254.169.254 云元数据）
        if (in(0xA9FE0000u, 16)) return true;
        // 100.64.0.0/10（CGNAT）
        if (in(0x64400000u, 10)) return true;
        // 224.0.0.0/4（组播）
        if (in(0xE0000000u, 4)) return true;
        // 240.0.0.0/4（保留，含 255.255.255.255 广播）
        if (in(0xF0000000u, 4)) return true;
        return false;
    }

    // IPv6
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        // QHostAddress::isLoopback 覆盖 ::1（且对 IPv4-mapped 回环也正确）。
        if (addr.isLoopback()) return true;
        // ::（未指定地址）
        if (addr == QHostAddress(QLatin1String("::"))) return true;
        const Q_IPV6ADDR a = addr.toIPv6Address();
        // fc00::/7（唯一本地地址 ULA）：首字节高 7 位为 1111110 → 首字节 0xfc 或 0xfd
        if ((a[0] & 0xFE) == 0xFC) return true;
        // fe80::/10（链路本地）：首 10 位 1111111010
        if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return true;
        // ff00::/8（组播）
        if (a[0] == 0xFF) return true;
        return false;
    }

    return false;
}

// 域名 DNS 判定（提交期 UrlGuard 与加载期 SsrfInterceptor 共用，避免策略漂移）。
enum class HostDnsVerdict {
    Allow,            // 解析成功且全部地址公网
    BlockPrivate,     // 解析成功且至少一个地址命中私网/回环（badAddress 为首个命中者）
    BlockUnresolved,  // DNS 解析失败（errorString 为错误描述）
};
struct HostDnsResult {
    HostDnsVerdict verdict = HostDnsVerdict::Allow;
    QHostAddress badAddress;   // BlockPrivate 时命中的首个内网地址
    QString errorString;       // BlockUnresolved 时解析错误描述
};

// 对域名做同步 DNS 解析并判定网段。跳过未指定地址（:: / 0.0.0.0，不可路由），
// 任一地址命中 isPrivateOrLoopbackAddress 即 BlockPrivate。QHostInfo::fromName 阻塞，
// 仅在工作线程或 Chromium IO 线程调用。
inline HostDnsResult checkHostDns(const QString& host) {
    HostDnsResult r;
    const QHostInfo info = QHostInfo::fromName(host);
    if (info.error() != QHostInfo::NoError) {
        r.verdict = HostDnsVerdict::BlockUnresolved;
        r.errorString = info.errorString();
        return r;
    }
    for (const QHostAddress& a : info.addresses()) {
        if (a == QHostAddress(QLatin1String("::"))
            || a == QHostAddress(QLatin1String("0.0.0.0"))) {
            continue;  // 未指定地址不可路由，跳过（字面量分支仍拦截它们）
        }
        if (isPrivateOrLoopbackAddress(a)) {
            r.verdict = HostDnsVerdict::BlockPrivate;
            r.badAddress = a;
            return r;
        }
    }
    return r;  // Allow
}

// 综合 SSRF 判定：URL 是否可安全渲染。
// 先对 host 字面量（可能是 IP）判；若是域名，再经 checkHostDns 解析判定。
// 返回空串表示通过；非空串为拒绝原因（可回传给客户端）。
inline std::string urlSsrfCheck(const QUrl& url) {
    if (!url.isValid()) return "invalid url";
    QString host = url.host().trimmed();
    if (host.isEmpty()) return "missing host";

    // 第一层：host 本身是 IP 字面量。
    QHostAddress literal(host);
    if (!literal.isNull()) {
        if (isPrivateOrLoopbackAddress(literal)) {
            return ("target IP " + host.toStdString()
                    + " is private/loopback/link-local (blocked by SSRF guard)");
        }
        return std::string();  // 公网 IP 字面量，放行
    }

    // 第二层：域名，复用 checkHostDns（与加载期 SsrfInterceptor 同一判定，防策略漂移）。
    const HostDnsResult r = checkHostDns(host);
    switch (r.verdict) {
        case HostDnsVerdict::BlockUnresolved:
            // 解析失败：不借此放行（避免攻击者用无法解析的域名绕过），保守拒绝。
            return ("DNS resolution failed for " + host.toStdString()
                    + " (" + r.errorString.toStdString() + ")");
        case HostDnsVerdict::BlockPrivate:
            return ("target " + host.toStdString() + " resolves to private/loopback "
                    + r.badAddress.toString().toStdString() + " (blocked by SSRF guard)");
        case HostDnsVerdict::Allow:
            return std::string();  // 全部解析结果均为公网，放行
    }
    return std::string();
}

} // namespace seimi
