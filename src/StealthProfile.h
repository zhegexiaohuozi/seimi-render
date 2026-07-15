#pragma once

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace seimi {

// 浏览器指纹统一配置（Tor 式「融入人群」）。
// 所有渲染实例伪装成同一个 Chrome on Windows 10 桌面环境，取值与 stealth.js 一致。
// 三层联动必须一致，否则 JS/HTTP 不一致本身是强反爬信号：
//   - JS 层（stealth.js，DocumentCreation 注入 MainWorld）
//   - HTTP 头层（SsrfRequestInterceptor::interceptRequest）
//   - HTTP UA（QWebEngineProfile::setHttpUserAgent，见 RenderPool::start）
struct StealthProfile {
    // —— Chrome 142 / Windows 10 ——
    static const char* userAgent() {
        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
               "(KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36";
    }
    // Client Hints（Sec-CH-UA 请求头）
    static const char* secChUa() {
        return "\"Google Chrome\";v=\"142\", \"Chromium\";v=\"142\", \"Not_A Brand\";v=\"24\"";
    }
    static const char* secChUaPlatform() { return "\"Windows\""; }
    static const char* acceptLanguage() { return "zh-CN,zh;q=0.9,en;q=0.8"; }
    static const char* acceptEncoding() { return "gzip, deflate, br"; }

    // 统一窗口尺寸（与 screen.width/height 指纹一致）
    static constexpr int viewportWidth() { return 1920; }
    static constexpr int viewportHeight() { return 1080; }

    // 从磁盘加载 stealth.js（指纹伪装脚本主体）。
    // 路径优先级同 simplify.js：二进制同级 third_party/stealth/stealth.js，
    // 开发期回退源码目录（SEIMI_ADMIN_UI_SRC_DIR/../third_party/stealth/）。
    // 找不到返回空（调用方静默降级，渲染继续但无指纹伪装）。
    static QString loadStealthJs() {
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/stealth/stealth.js"),
#ifdef SEIMI_ADMIN_UI_SRC_DIR
            QStringLiteral(SEIMI_ADMIN_UI_SRC_DIR) + QStringLiteral("/../third_party/stealth/stealth.js"),
#endif
        };
        for (const QString& path : candidates) {
            QFileInfo fi(path);
            if (!fi.isFile()) continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return QString::fromUtf8(f.readAll());
            }
        }
        return {};
    }
};

} // namespace seimi
