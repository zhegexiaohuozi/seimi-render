// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../AppConfig.h"
#include "../ProxyConfig.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace seimi {

// 设置页模型（QML 单例）：
//   - effective：当前生效值（defaults < config.json < CLI 合并后的 Config 快照），
//     页面初始化表单用；
//   - cliFlags：哪些字段被 CLI 显式覆盖（GUI 写文件时跳过它们，CLI 本次启动继续胜出）；
//   - save(form)：把表单写回 config.json；代理字段可选立即热生效（ProxyConfig::setProxy，
//     RenderPool 的 500ms 定时器完成 apply，无需重启）。
// 除代理外全部为启动期参数，改动需重启——save 返回 needsRestart 供 UI 提示。
class SettingsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap effective READ effective CONSTANT)
    Q_PROPERTY(QVariantMap cliFlags READ cliFlags CONSTANT)
public:
    SettingsModel(AppConfig* cfg, ProxyConfig* proxy,
                  const QVariantMap& effective, const QVariantMap& cliFlags,
                  QObject* parent = nullptr);

    QVariantMap effective() const { return m_effective; }
    QVariantMap cliFlags() const { return m_cliFlags; }

    // form 键与 effective 一致；返回 {ok:bool, changed:[str], proxyApplied:bool,
    // needsRestart:bool, error:str}。
    Q_INVOKABLE QVariantMap save(const QVariantMap& form, bool applyProxyNow);

private:
    AppConfig* m_cfg;      // GuiApp 持有，非所有权
    ProxyConfig* m_proxy;  // 可空（理论上必有）
    QVariantMap m_effective;
    QVariantMap m_cliFlags;
};

} // namespace seimi
