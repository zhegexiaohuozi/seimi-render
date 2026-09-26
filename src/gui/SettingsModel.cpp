// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "SettingsModel.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cstring>

namespace seimi {
namespace {

// CLI 覆盖的字段不写文件：CLI 在本次启动继续胜出；下次以无 CLI 方式启动时用文件值。
bool cliOwns(const QVariantMap& cliFlags, const char* key) {
    return cliFlags.value(QLatin1String(key)).toBool();
}

// 数值钳制（与 parseArgs 的下限一致，文件值也走同一护栏）。
int clampInt(const QVariant& v, int lo, int hi, int fallback) {
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? std::clamp(n, lo, hi) : fallback;
}

} // namespace

SettingsModel::SettingsModel(AppConfig* cfg, ProxyConfig* proxy,
                             const QVariantMap& effective, const QVariantMap& cliFlags,
                             QObject* parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_proxy(proxy)
    , m_effective(effective)
    , m_cliFlags(cliFlags)
{
}

QVariantMap SettingsModel::save(const QVariantMap& form, bool applyProxyNow) {
    QVariantMap result;
    QStringList changed;
    const bool hadProxy = m_cfg->proxy.has_value();

    auto note = [&](const char* key, bool differs) {
        if (differs) changed << QString::fromLatin1(key);
    };
    auto sameStr = [&](const QVariantMap& a, const QVariantMap& b, const char* k) {
        return a.value(QLatin1String(k)).toString() == b.value(QLatin1String(k)).toString();
    };
    auto sameNum = [&](const QVariantMap& a, const QVariantMap& b, const char* k) {
        return a.value(QLatin1String(k)).toInt() == b.value(QLatin1String(k)).toInt();
    };
    auto sameBool = [&](const QVariantMap& a, const QVariantMap& b, const char* k) {
        return a.value(QLatin1String(k)).toBool() == b.value(QLatin1String(k)).toBool();
    };

    // —— 字符串/数值/布尔字段 ——
    if (!cliOwns(m_cliFlags, "host")) {
        note("host", !sameStr(form, m_effective, "host"));
        m_cfg->host = form.value(QLatin1String("host")).toString().trimmed();
    }
    struct NumField { const char* key; int lo, hi; };
    const NumField nums[] = {
        {"httpPort", 1, 65535}, {"wsPort", 1, 65535}, {"mcpPort", 1, 65535},
        {"concurrency", 1, 64}, {"httpThreads", 2, 128},
        {"settleMs", 0, 600000}, {"loadTimeoutMs", 1000, 600000},
        {"remoteDebugPort", 0, 65535},
    };
    for (const NumField& nf : nums) {
        const int v = clampInt(form.value(QLatin1String(nf.key)), nf.lo, nf.hi,
                               m_effective.value(QLatin1String(nf.key)).toInt());
        note(nf.key, !sameNum(form, m_effective, nf.key));
        if (strcmp(nf.key, "loadTimeoutMs") == 0) m_cfg->loadTimeoutMs = qint64(v);
        else if (strcmp(nf.key, "httpPort") == 0) m_cfg->httpPort = v;
        else if (strcmp(nf.key, "wsPort") == 0) m_cfg->wsPort = v;
        else if (strcmp(nf.key, "mcpPort") == 0) m_cfg->mcpPort = v;
        else if (strcmp(nf.key, "concurrency") == 0) m_cfg->concurrency = v;
        else if (strcmp(nf.key, "httpThreads") == 0) m_cfg->httpThreads = v;
        else if (strcmp(nf.key, "settleMs") == 0) m_cfg->settleMs = v;
        else if (strcmp(nf.key, "remoteDebugPort") == 0) m_cfg->remoteDebugPort = v;
    }
    if (!cliOwns(m_cliFlags, "password")) {
        note("password", !sameStr(form, m_effective, "password"));
        m_cfg->password = form.value(QLatin1String("password")).toString();
    }
    struct BoolField { const char* key; std::optional<bool> AppConfig::*member; };
    const BoolField bools[] = {
        {"adminEnabled", &AppConfig::adminEnabled},
        {"ssrfGuard", &AppConfig::ssrfGuard},
        {"stealth", &AppConfig::stealth},
        {"warmup", &AppConfig::warmup},
    };
    for (const BoolField& bf : bools) {
        note(bf.key, !sameBool(form, m_effective, bf.key));
        m_cfg->*bf.member = form.value(QLatin1String(bf.key)).toBool();
    }
    if (!cliOwns(m_cliFlags, "trustedProxies")) {
        note("trustedProxies", !sameStr(form, m_effective, "trustedProxies"));
        m_cfg->trustedProxies = form.value(QLatin1String("trustedProxies")).toString().trimmed();
    }
    if (!cliOwns(m_cliFlags, "warmupUrl")) {
        note("warmupUrl", !sameStr(form, m_effective, "warmupUrl"));
        m_cfg->warmupUrl = form.value(QLatin1String("warmupUrl")).toString().trimmed();
    }
    if (!cliOwns(m_cliFlags, "proxyBypass")) {
        note("proxyBypass", !sameStr(form, m_effective, "proxyBypass"));
        m_cfg->proxyBypass = form.value(QLatin1String("proxyBypass")).toString().trimmed();
    }

    // —— 数据目录（一跳重定向，重启后生效；空 = 恢复默认）——
    // 写前实检：目录可建且可写，防止下次启动落进一个起不来的死目录。
    if (!cliOwns(m_cliFlags, "dataDir")) {
        const QString newDir = form.value(QLatin1String("dataDir")).toString().trimmed();
        if (!newDir.isEmpty()) {
            if (!QDir(newDir).exists() && !QDir().mkpath(newDir)) {
                result.insert(QStringLiteral("ok"), false);
                result.insert(QStringLiteral("error"),
                              QStringLiteral("数据目录创建失败: %1").arg(newDir));
                return result;
            }
            if (!QFileInfo(newDir).isWritable()) {
                result.insert(QStringLiteral("ok"), false);
                result.insert(QStringLiteral("error"),
                              QStringLiteral("数据目录不可写: %1").arg(newDir));
                return result;
            }
        }
        note("dataDir", newDir != m_effective.value(QLatin1String("dataDir")).toString());
        m_cfg->dataDirOverride = newDir.isEmpty()
            ? std::nullopt
            : std::optional<QString>(QDir(newDir).absolutePath());
    }

    // —— 代理：组装 ProxySpec（type/host/port/user/pass）——
    ProxySpec spec;
    const QString ptype = form.value(QLatin1String("proxyType")).toString();
    if (ptype == QLatin1String("http")) spec.type = ProxySpec::Type::Http;
    else if (ptype == QLatin1String("socks5")) spec.type = ProxySpec::Type::Socks5;
    else spec.type = ProxySpec::Type::Direct;
    spec.host = form.value(QLatin1String("proxyHost")).toString().trimmed().toStdString();
    spec.port = quint16(clampInt(form.value(QLatin1String("proxyPort")), 0, 65535, 0));
    spec.user = form.value(QLatin1String("proxyUser")).toString().toStdString();
    spec.pass = form.value(QLatin1String("proxyPass")).toString().toStdString();
    if (spec.type == ProxySpec::Type::Direct || spec.host.empty() || spec.port == 0) {
        spec = ProxySpec(); // 不完整配置容错为直连（与 CLI --proxy 语义一致）
    }
    if (!cliOwns(m_cliFlags, "proxy")) {
        note("proxy", hadProxy ? !(*m_cfg->proxy == spec) : spec.type != ProxySpec::Type::Direct);
        m_cfg->proxy = spec;
    }

    // —— 落盘 ——
    const bool ok = m_cfg->save();

    // —— 代理热生效（不等重启；RenderPool 定时器完成实际 apply）——
    bool proxyApplied = false;
    if (applyProxyNow && m_proxy && !cliOwns(m_cliFlags, "proxy")) {
        m_proxy->setProxy(spec);
        proxyApplied = true;
    }

    // 代理字段不触发「需重启」；其余任何改动都要重启才生效。
    QStringList restartKeys(changed);
    restartKeys.removeAll(QStringLiteral("proxy"));
    const bool needsRestart = !restartKeys.isEmpty();

    result.insert(QStringLiteral("ok"), ok);
    result.insert(QStringLiteral("changed"), changed);
    result.insert(QStringLiteral("proxyApplied"), proxyApplied);
    result.insert(QStringLiteral("needsRestart"), needsRestart);
    if (!ok) result.insert(QStringLiteral("error"),
                           QStringLiteral("写入 config.json 失败（目录只读？），详见控制台日志。"));
    return result;
}

} // namespace seimi
