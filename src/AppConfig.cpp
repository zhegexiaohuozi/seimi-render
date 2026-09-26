// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "AppConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <cstdio>

namespace seimi {

QJsonObject proxySpecToJson(const ProxySpec& spec) {
    QJsonObject obj;
    const char* type = spec.type == ProxySpec::Type::Socks5 ? "socks5"
                     : spec.type == ProxySpec::Type::Http   ? "http"
                                                            : "direct";
    obj.insert(QStringLiteral("type"), QLatin1String(type));
    obj.insert(QStringLiteral("host"), QString::fromStdString(spec.host));
    obj.insert(QStringLiteral("port"), int(spec.port));
    obj.insert(QStringLiteral("user"), QString::fromStdString(spec.user));
    obj.insert(QStringLiteral("pass"), QString::fromStdString(spec.pass));
    return obj;
}

std::optional<ProxySpec> proxySpecFromJson(const QJsonObject& obj) {
    if (obj.isEmpty()) return std::nullopt;
    const QString type = obj.value(QStringLiteral("type")).toString(QStringLiteral("direct"));
    ProxySpec spec;
    if (type == QLatin1String("direct")) {
        return spec; // Direct：其余字段忽略
    }
    if (type == QLatin1String("http")) {
        spec.type = ProxySpec::Type::Http;
    } else if (type == QLatin1String("socks5")) {
        spec.type = ProxySpec::Type::Socks5;
    } else {
        return std::nullopt; // 非法类型：整段丢弃（回退 CLI/默认），不猜
    }
    spec.host = obj.value(QStringLiteral("host")).toString().toStdString();
    spec.port = quint16(obj.value(QStringLiteral("port")).toInt());
    spec.user = obj.value(QStringLiteral("user")).toString().toStdString();
    spec.pass = obj.value(QStringLiteral("pass")).toString().toStdString();
    if (spec.host.empty() || spec.port == 0) {
        return ProxySpec(); // 有 type 但缺 host/port：容错为直连（与 parseProxyUrl 语义一致）
    }
    return spec;
}

AppConfig AppConfig::load(const QString& dataDir) {
    AppConfig cfg;
    cfg.m_dataDir = dataDir;
    cfg.m_filePath = dataDir + QStringLiteral("/config.json");

    QFile f(cfg.m_filePath);
    if (!f.exists()) return cfg; // 首次启动：全默认
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "[WARN] config.json 不可读（权限？），使用默认配置: %s\n",
                     qPrintable(cfg.m_filePath));
        return cfg;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::fprintf(stderr,
                     "[WARN] config.json 解析失败（%s），已忽略整份文件使用默认配置。\n"
                     "       修复或删除该文件后重启: %s\n",
                     qPrintable(err.errorString()), qPrintable(cfg.m_filePath));
        return cfg;
    }
    const QJsonObject root = doc.object();

    // —— server 段：只认已知键，全部宽容解析（坏值跳过，绝不因配置文件起不来）——
    const QJsonObject server = root.value(QStringLiteral("server")).toObject();
    auto optStr = [&](const char* key) -> std::optional<QString> {
        const QJsonValue v = server.value(QLatin1String(key));
        return v.isString() ? std::optional<QString>(v.toString()) : std::nullopt;
    };
    auto optInt = [&](const char* key) -> std::optional<int> {
        const QJsonValue v = server.value(QLatin1String(key));
        return v.isDouble() ? std::optional<int>(v.toInt()) : std::nullopt;
    };
    auto optInt64 = [&](const char* key) -> std::optional<qint64> {
        const QJsonValue v = server.value(QLatin1String(key));
        return v.isDouble() ? std::optional<qint64>(qint64(v.toDouble())) : std::nullopt;
    };
    auto optBool = [&](const char* key) -> std::optional<bool> {
        const QJsonValue v = server.value(QLatin1String(key));
        return v.isBool() ? std::optional<bool>(v.toBool()) : std::nullopt;
    };

    cfg.host             = optStr("host");
    cfg.httpPort         = optInt("http_port");
    cfg.wsPort           = optInt("ws_port");
    cfg.mcpPort          = optInt("mcp_port");
    cfg.concurrency      = optInt("concurrency");
    cfg.httpThreads      = optInt("http_threads");
    cfg.settleMs         = optInt("settle_ms");
    cfg.loadTimeoutMs    = optInt64("load_timeout_ms");
    cfg.password         = optStr("password");
    cfg.adminEnabled     = optBool("admin_ui");
    cfg.ssrfGuard        = optBool("ssrf_guard");
    cfg.trustedProxies   = optStr("trusted_proxies");
    if (server.contains(QStringLiteral("proxy"))) {
        // 显式出现的 proxy 段才解析；direct 表示「文件明确要求直连」。
        const QJsonValue v = server.value(QStringLiteral("proxy"));
        if (v.isObject()) cfg.proxy = proxySpecFromJson(v.toObject());
    }
    cfg.proxyBypass      = optStr("proxy_bypass");
    cfg.stealth          = optBool("stealth");
    cfg.warmup           = optBool("warmup");
    cfg.warmupUrl        = optStr("warmup_url");
    cfg.remoteDebugPort  = optInt("remote_debug_port");
    cfg.dataDirOverride  = optStr("data_dir");

    // —— gui 段：坏值回退默认 ——
    const QJsonObject gui = root.value(QStringLiteral("gui")).toObject();
    const QString theme = gui.value(QStringLiteral("theme")).toString();
    if (theme == QLatin1String("light") || theme == QLatin1String("dark")
        || theme == QLatin1String("system")) {
        cfg.gui.theme = theme;
    }
    const QString locale = gui.value(QStringLiteral("locale")).toString();
    if (locale == QLatin1String("zh") || locale == QLatin1String("en")) {
        cfg.gui.locale = locale;
    }
    cfg.gui.collapseNav     = gui.value(QStringLiteral("collapse_nav")).toBool(false);
    cfg.gui.reducedMotion   = gui.value(QStringLiteral("reduced_motion")).toBool(false);
    const QString channel   = gui.value(QStringLiteral("update_channel")).toString();
    if (channel == QLatin1String("stable") || channel == QLatin1String("beta")) {
        cfg.gui.updateChannel = channel;
    }
    cfg.gui.autoCheckUpdates = gui.value(QStringLiteral("auto_check")).toBool(true);
    cfg.gui.manifestUrl      = gui.value(QStringLiteral("manifest_url")).toString().trimmed();

    return cfg;
}

bool AppConfig::save() const {
    QDir().mkpath(m_dataDir);
    QJsonObject server;
    auto putStr = [&](const char* key, const std::optional<QString>& v) {
        if (v.has_value()) server.insert(QLatin1String(key), *v);
    };
    auto putInt = [&](const char* key, const std::optional<int>& v) {
        if (v.has_value()) server.insert(QLatin1String(key), *v);
    };
    auto putInt64 = [&](const char* key, const std::optional<qint64>& v) {
        if (v.has_value()) server.insert(QLatin1String(key), double(*v));
    };
    auto putBool = [&](const char* key, const std::optional<bool>& v) {
        if (v.has_value()) server.insert(QLatin1String(key), *v);
    };
    putStr("host", host);
    putInt("http_port", httpPort);
    putInt("ws_port", wsPort);
    putInt("mcp_port", mcpPort);
    putInt("concurrency", concurrency);
    putInt("http_threads", httpThreads);
    putInt("settle_ms", settleMs);
    putInt64("load_timeout_ms", loadTimeoutMs);
    putStr("password", password);
    putBool("admin_ui", adminEnabled);
    putBool("ssrf_guard", ssrfGuard);
    putStr("trusted_proxies", trustedProxies);
    if (proxy.has_value()) server.insert(QStringLiteral("proxy"), proxySpecToJson(*proxy));
    putStr("proxy_bypass", proxyBypass);
    putBool("stealth", stealth);
    putBool("warmup", warmup);
    putStr("warmup_url", warmupUrl);
    putInt("remote_debug_port", remoteDebugPort);
    putStr("data_dir", dataDirOverride);

    QJsonObject guiObj;
    guiObj.insert(QStringLiteral("theme"), gui.theme);
    guiObj.insert(QStringLiteral("locale"), gui.locale);
    guiObj.insert(QStringLiteral("collapse_nav"), gui.collapseNav);
    guiObj.insert(QStringLiteral("reduced_motion"), gui.reducedMotion);
    guiObj.insert(QStringLiteral("update_channel"), gui.updateChannel);
    guiObj.insert(QStringLiteral("auto_check"), gui.autoCheckUpdates);
    if (!gui.manifestUrl.isEmpty()) {
        guiObj.insert(QStringLiteral("manifest_url"), gui.manifestUrl);
    }

    QJsonObject root;
    root.insert(QStringLiteral("server"), server);
    root.insert(QStringLiteral("gui"), guiObj);

    // 原子写：QSaveFile 提交即 rename（同 Metrics 模式），写一半崩溃不留半份配置。
    QSaveFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "[WARN] config.json 打不开写: %s\n", qPrintable(m_filePath));
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        std::fprintf(stderr, "[WARN] config.json 写入失败: %s\n", qPrintable(m_filePath));
        return false;
    }
    return true;
}

} // namespace seimi
