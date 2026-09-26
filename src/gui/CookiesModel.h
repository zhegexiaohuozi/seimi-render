// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../CookieStore.h"

#include <QAbstractListModel>
#include <QObject>

namespace seimi {

// 登录态页模型（QML 单例）：域名→数量列表 + 清理会话/永久删除/落盘动作。
// 数据来自 CookieStore::snapshot()（线程安全）；动作直接调 CookieStore（record/clear
// 系列任意线程安全）。1s 轮询刷新 + Q_INVOKABLE 手动刷新。
class CookiesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int domainCount READ domainCount NOTIFY changed)
public:
    enum Roles {
        DomainRole = Qt::UserRole + 1,
        CountRole
    };

    explicit CookiesModel(CookieStore* store, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int total() const { return m_total; }
    int domainCount() const { return int(m_rows.size()); }

    Q_INVOKABLE void refresh();
    // 清空当前会话（内存 + 下次 apply 清 profile cookies；不动加密文件）。
    Q_INVOKABLE void clearSession();
    // 永久删除：销毁 data/cookies.dat 加密文件 + 全部内存状态（危险操作，UI 双确认）。
    Q_INVOKABLE void purgePermanent();
    // 强制把 dirty 仓库落盘。
    Q_INVOKABLE void flushStore();

signals:
    void changed();

private:
    struct Row {
        QString domain;
        int count = 0;
    };
    void poll();

    CookieStore* m_store;
    QList<Row> m_rows;
    int m_total = 0;
    int m_knownTotal = -1; // 变更检测（CookieStore 无变更信号，轮询比对）
};

} // namespace seimi
