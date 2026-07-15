// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SSRF 二次防护：在 Chromium 实际发起每个请求时再判定，堵住「提交时校验、加载时绕过」的旁路：
//   1) HTTP 重定向：公网 URL 302 跳内网/云元数据，Chromium 自动跟随。
//   2) DNS rebinding（TOCTOU）：提交期解析与加载期解析间切换到内网 IP。
//   3) 内嵌子框架：<iframe src=内网> 经 screenshot 外泄像素。
//
// interceptRequest 跑在 Chromium IO 线程，必须快：
//   - IP 字面量：对所有请求类型做（廉价、同步），命中私网即 block。
//   - 域名：仅对导航类（主/子框架）+ XHR + Ping 做 DNS 解析校验（响应会被回传或可盲探内网）；
//     img/css/script 等海量子资源跳过，避免 IO 线程逐个阻塞解析。
//   - 非 http/https/data/about/blob 方案（file/ftp 等）一律拦截。
//
// 残留风险：img/css/script/font 子资源域名不做 DNS 校验，但其响应跨域不可读，外泄面有限。
//
// 仅依赖 Qt，header-only，无 Q_OBJECT。
//

#include "UrlGuard.h"

#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

#include <QHostAddress>
#include <QHostInfo>
#include <QString>
#include <QUrl>

namespace seimi {

class SsrfRequestInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    // stealthHeaders: 开启时对所有 http(s) 请求注入统一的指纹相关头
    //（Sec-CH-UA / Accept-Language 等），与 stealth.js 的 JS 指纹保持一致。
    explicit SsrfRequestInterceptor(bool stealthHeaders = false, QObject* parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent)
        , m_stealthHeaders(stealthHeaders) {}

    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        const QUrl url = info.requestUrl();
        const QString scheme = url.scheme();

        // stealth 指纹头统一：对 http(s) 注入 Chrome 桌面一致头（与 JS 层 userAgentData 一致）。
        if (m_stealthHeaders && (scheme == QLatin1String("http")
                                 || scheme == QLatin1String("https"))) {
            info.setHttpHeader(QByteArrayLiteral("Accept-Language"),
                               QByteArrayLiteral("zh-CN,zh;q=0.9,en;q=0.8"));
            info.setHttpHeader(QByteArrayLiteral("Sec-CH-UA"),
                               QByteArrayLiteral("\"Google Chrome\";v=\"142\", \"Chromium\";v=\"142\", \"Not_A Brand\";v=\"24\""));
            info.setHttpHeader(QByteArrayLiteral("Sec-CH-UA-Mobile"), QByteArrayLiteral("?0"));
            info.setHttpHeader(QByteArrayLiteral("Sec-CH-UA-Platform"), QByteArrayLiteral("\"Windows\""));
        }

        // 方案白名单：仅放行有网络/内联语义的安全方案，其余（file/ftp/chrome 等）拦截。
        const bool isHttp = (scheme == QLatin1String("http")
                             || scheme == QLatin1String("https"));
        const bool isInlineSafe = (scheme == QLatin1String("data")
                                   || scheme == QLatin1String("about")
                                   || scheme == QLatin1String("blob"));
        if (!isHttp && !isInlineSafe) {
            info.block(true);
            return;
        }
        if (!isHttp) return;  // data/about/blob 无外部网络目标，放行

        const QString host = url.host().trimmed();
        if (host.isEmpty()) return;

        // 1) host 是 IP 字面量：所有请求类型都判（同步、廉价）。
        QHostAddress literal(host);
        if (!literal.isNull()) {
            // 字面量 :: / 0.0.0.0 不可路由，跳过（与 UrlGuard DNS 解析逻辑一致）
            if (!(literal == QHostAddress(QStringLiteral("::"))
                  || literal == QHostAddress(QStringLiteral("0.0.0.0")))) {
                if (isPrivateOrLoopbackAddress(literal)) info.block(true);
            }
            return;
        }

        // 2) host 是域名：仅对导航类 + XHR + Ping 做 DNS 校验（响应可回传/可盲探内网），
        //    img/css/script 等海量子资源跳过（IO 线程逐个解析代价过高）。
        const auto rt = info.resourceType();
        const bool isDnsChecked =
            rt == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
            || rt == QWebEngineUrlRequestInfo::ResourceTypeSubFrame
            || rt == QWebEngineUrlRequestInfo::ResourceTypeXhr
            || rt == QWebEngineUrlRequestInfo::ResourceTypePing;
        if (!isDnsChecked) return;

        // 复用 UrlGuard::checkHostDns（提交期同一真相源）：解析失败或任一地址命中私网即 block。
        const HostDnsResult r = checkHostDns(host);
        if (r.verdict != HostDnsVerdict::Allow) {
            info.block(true);
        }
    }

private:
    bool m_stealthHeaders;
};

} // namespace seimi

