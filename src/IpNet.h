// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// IP 网段（CIDR）判定 + 反向代理真实客户端 IP 解析。
//
// 用途：--trusted-proxy 接受「精确 IP 或 CIDR」列表，据此从 X-Forwarded-For 还原
// 真实客户端 IP，供 /api/login 按真实 IP 限流。
//
// 安全语义同 nginx set_real_ip_from：XFF 可被任意客户端伪造，仅当【直连来源】
// 落在可信代理网段时才信任其 XFF，并从 XFF 链右端逐个剥离可信代理取真实客户端。
// IPv4-mapped IPv6（::ffff:a.b.c.d）规整为 IPv4 参与判定。
//
// 仅依赖 Qt（QHostAddress），header-only。
//

#include <QHostAddress>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace seimi {

// 一个 CIDR 网段（或单个 IP，等价于 /32 或 /128）。
// 同时持有原文本用于错误提示。host 段统一规整：IPv4 存为 IPv4，mapped 也规整为 IPv4。
struct IpNet {
    QHostAddress host;
    int prefix = 0;          // 前缀长度；IPv4 /32、IPv6 /128
    bool valid = false;
    QString raw;             // 原始输入文本
};

namespace detail {

// 把 IPv4-mapped IPv6（::ffff:a.b.c.d）规整为 IPv4。
inline QHostAddress normalizeAddr(const QHostAddress& a) {
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR b = a.toIPv6Address();
        // 用字节 [10]==0xff && [11]==0xff 判定 mapped（不能靠 toIPv4Address()!=0：
        // ::ffff:0.0.0.0 的嵌入值为 0，与 :: 同为 0 会误判）。
        if (b[10] == 0xff && b[11] == 0xff) {
            return QHostAddress(a.toIPv4Address());
        }
    }
    return a;
}

// 单个地址是否落入某网段（同协议前提下）。
inline bool addrInNet(const QHostAddress& addr, const IpNet& net) {
    const QHostAddress a = normalizeAddr(addr);
    const QHostAddress base = normalizeAddr(net.host);
    if (a.protocol() != base.protocol()) return false;

    if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 av = a.toIPv4Address();
        const quint32 bv = base.toIPv4Address();
        const quint32 mask = net.prefix >= 32 ? 0xffffffffu
                                              : (~0u << (32 - net.prefix));
        return (av & mask) == (bv & mask);
    }
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR av = a.toIPv6Address();
        const Q_IPV6ADDR bv = base.toIPv6Address();
        int bits = net.prefix;
        // 按字节比对前 prefix 位。
        for (int i = 0; i < 16 && bits > 0; ++i) {
            if (bits >= 8) {
                if (av[i] != bv[i]) return false;
                bits -= 8;
            } else {
                const quint8 mask = quint8(0xff << (8 - bits));
                if ((av[i] & mask) != (bv[i] & mask)) return false;
                bits = 0;
            }
        }
        return true;
    }
    return false;
}

} // namespace detail

// 解析单个 token（"1.2.3.4" 或 "10.0.0.0/8"）为 IpNet。无法解析时 valid=false。
inline IpNet parseIpNet(const QString& token) {
    IpNet net;
    net.raw = token.trimmed();
    if (net.raw.isEmpty()) return net;

    int slash = net.raw.indexOf(QLatin1Char('/'));
    QString hostPart = slash >= 0 ? net.raw.left(slash) : net.raw;
    hostPart = hostPart.trimmed();
    QHostAddress h(hostPart);
    if (h.isNull()) return net;  // 非法地址

    int prefix = -1;
    if (slash >= 0) {
        bool ok = false;
        prefix = net.raw.mid(slash + 1).trimmed().toInt(&ok);
        if (!ok || prefix < 0) return net;
    }
    if (prefix < 0) {
        prefix = (h.protocol() == QAbstractSocket::IPv4Protocol) ? 32 : 128;
    }
    const int maxPrefix = (h.protocol() == QAbstractSocket::IPv4Protocol) ? 32 : 128;
    if (prefix > maxPrefix) return net;

    net.host = h;
    net.prefix = prefix;
    net.valid = true;
    return net;
}

// 解析逗号分隔列表（"10.8.0.0/16,127.0.0.1"）。跳过非法项（不致命：容忍个别错配）。
inline std::vector<IpNet> parseIpNetList(const std::string& csv) {
    std::vector<IpNet> out;
    for (const QString& t : QString::fromStdString(csv).split(QLatin1Char(','),
                                                              Qt::SkipEmptyParts)) {
        IpNet net = parseIpNet(t);
        if (net.valid) out.push_back(net);
    }
    return out;
}

// 单个地址是否落入任一网段。
inline bool addrInAnyNet(const QHostAddress& addr, const std::vector<IpNet>& nets) {
    for (const IpNet& n : nets) {
        if (n.valid && detail::addrInNet(addr, n)) return true;
    }
    return false;
}

// 从 XFF 头提取真实客户端 IP（同 nginx real_ip）：
//   - 直连来源 remoteAddr 必须命中可信代理，否则返回 remoteAddr（不信任 XFF）。
//   - 命中时从 XFF 链【右端】剥离可信代理，取剩余最右一个为真实客户端。
//   - XFF 缺失/全空/全为可信代理：回退 remoteAddr。
inline QString resolveRealClientIp(const QHostAddress& remoteAddr,
                                   const QString& xForwardedFor,
                                   const std::vector<IpNet>& trustedProxies) {
    // 非可信代理来源：XFF 不可信，直接用直连地址。
    if (trustedProxies.empty() || !addrInAnyNet(remoteAddr, trustedProxies)) {
        return remoteAddr.toString();
    }

    const QStringList segs = xForwardedFor.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (segs.isEmpty()) return remoteAddr.toString();

    // 从右往左剥离可信代理。链如 "client, proxy1, proxy2"，
    // 剥光所有可信代理后取最右剩余项。
    for (int i = segs.size() - 1; i >= 0; --i) {
        const QHostAddress a(segs[i].trimmed());
        if (a.isNull()) {
            // 非法段：无法判定，保守取该段原文（避免误把非法但真实的 IP 丢弃）。
            return segs[i].trimmed();
        }
        if (!addrInAnyNet(a, trustedProxies)) {
            return a.toString();
        }
    }
    // 全是可信代理（说明链只有代理，没有客户端）：回退直连地址。
    return remoteAddr.toString();
}

} // namespace seimi
