// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ProxyConfig.h"

#include <QString>

#include <cstdint>
#include <optional>

class QJsonObject;

namespace seimi {

// 配置持久化层：data/config.json。
//
// 背景：此前全部参数为 CLI 时点参数，GUI 设置页要「管理」参数必须有落盘配置。
// 优先级（高 → 低）：CLI 显式 flag > config.json > 内置默认。
//   - main.cpp parseArgs 记录哪些 flag 被显式传入（AppConfig::CliOverrides）；
//   - main.cpp 的合并步骤只把「文件里有 且 CLI 未显式覆盖」的字段叠加进 Config
//     （逐字段 if，见 main.cpp mergeAppConfig）；
//   - GUI 设置页保存时写文件（CLI 已覆盖的字段不写，让 CLI 在本次启动继续胜出）。
//
// 线程模型：启动期单线程读写；GUI 运行期仅 GUI 线程读写（SettingsModel/GuiPrefs
// 均在 GUI 线程），无需加锁。写入为原子写（tmp + rename，同 Metrics 落盘模式）。
//
// 安全注意：server.password 为明文落盘（等价于 --password 的暴露面，比 ps 可见
// 的命令行略好、比 cookies.dat 的加密存储差）。文档中明示；不接受明文的用户可
// 改用 --password-file / SEIMI_PASSWORD，两者优先级均高于本文件。
class AppConfig {
public:
    // GUI 偏好（gui.* 命名空间，GUI 线程即时生效）。
    struct Gui {
        QString theme = QStringLiteral("system");     // system | light | dark
        QString locale = QStringLiteral("zh");        // zh | en
        bool collapseNav = false;                     // 侧栏折叠为纯图标
        bool reducedMotion = false;                   // 关闭非必要动效（无障碍）
        QString updateChannel = QStringLiteral("stable"); // stable | beta
        bool autoCheckUpdates = true;                 // 启动后自动检查更新
        QString manifestUrl;                          // 更新清单地址；空 = 未配置
    };

    // parseArgs 记录「CLI 显式传入」的字段集合（合并与 GUI 保存都依赖它）。
    struct CliOverrides {
        bool host = false, httpPort = false, wsPort = false, mcpPort = false;
        bool concurrency = false, httpThreads = false, settleMs = false, loadTimeoutMs = false;
        bool password = false, adminEnabled = false, ssrfGuard = false;
        bool trustedProxies = false, proxy = false, proxyBypass = false;
        bool stealth = false, warmup = false, warmupUrl = false, remoteDebugPort = false;
        bool dataDir = false;
        bool any() const {
            return host || httpPort || wsPort || mcpPort || concurrency || httpThreads
                || settleMs || loadTimeoutMs || password || adminEnabled || ssrfGuard
                || trustedProxies || proxy || proxyBypass || stealth || warmup
                || warmupUrl || remoteDebugPort || dataDir;
        }
    };

    // dataDir：config.json 所在目录（bin 同级 data/）。目录/文件不存在则全部返回
    // nullopt（纯默认行为，与无此文件完全一致）；JSON 损坏同样回退默认（告警不致命）。
    static AppConfig load(const QString& dataDir);

    // 原子写回 dataDir/config.json。失败返回 false（调用方负责提示）。
    bool save() const;

    QString filePath() const { return m_filePath; }
    QString dataDir() const { return m_dataDir; }

    // ---------- server 段（nullopt = 文件未指定该字段；GUI 保存时按需 set/reset）----------
    std::optional<QString> host;
    std::optional<int> httpPort;
    std::optional<int> wsPort;
    std::optional<int> mcpPort;
    std::optional<int> concurrency;
    std::optional<int> httpThreads;
    std::optional<int> settleMs;
    std::optional<qint64> loadTimeoutMs;
    std::optional<QString> password;        // 明文（见类注释）
    std::optional<bool> adminEnabled;
    std::optional<bool> ssrfGuard;
    std::optional<QString> trustedProxies;
    std::optional<ProxySpec> proxy;         // 初始代理（GUI 代理卡持久化用）
    std::optional<QString> proxyBypass;
    std::optional<bool> stealth;
    std::optional<bool> warmup;
    std::optional<QString> warmupUrl;
    std::optional<int> remoteDebugPort;
    // 数据目录重定向（server.data_dir）：只影响数据落盘（cookies/metrics/anomalies/
    // updates），config.json 本身始终留在引导目录（bin/data 或 --data-dir）——这样
    // 重定向永远是「一跳」且无环。空串/nullopt = 默认（bin 同级 data/）。CLI
    // --data-dir 显式给定时整个机制让位（目录与数据同址，行为与既往一致）。
    // 命名避开成员函数 dataDir()。
    std::optional<QString> dataDirOverride;

    Gui gui;

private:
    QString m_dataDir;
    QString m_filePath;
};

// ProxySpec ↔ JSON（AppConfig 与 SettingsModel 共用）。
QJsonObject proxySpecToJson(const ProxySpec& spec);
// 解析失败（type 非法）返回 std::nullopt；host/port 缺失按 Direct 处理。
std::optional<ProxySpec> proxySpecFromJson(const QJsonObject& obj);

} // namespace seimi
