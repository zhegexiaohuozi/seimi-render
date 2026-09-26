// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QAbstractListModel>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

#include <deque>

namespace seimi {

// 日志桥 + 日志页模型（QML 单例）。
//
// 桥：全局静态环形缓冲（5000 条），main.cpp 的 Qt 消息处理器在「噪声过滤判定之后、
// fprintf 之前」调 LogModel::bridgeAppend——GUI 里看到的与控制台完全一致。
// 任意线程 append（httplib/MCP/Chromium 转发都可能非 GUI 线程），仅入 deque + 锁。
//
// 模型：GUI 线程 1s 轮询把环形缓冲搬进 QAbstractListModel（过滤在拷贝侧做），
// beginResetModel/endResetModel 刷新（行数有界，开销可忽略）。展示上限 3000 条
//（环形 5000，展示窗口取最新 3000，避免超长列表 delegate 开销）。
class LogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY changed)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY changed)
    Q_PROPERTY(bool showDebug READ showDebug WRITE setShowDebug NOTIFY changed)
    Q_PROPERTY(bool showWarning READ showWarning WRITE setShowWarning NOTIFY changed)
    Q_PROPERTY(bool showCritical READ showCritical WRITE setShowCritical NOTIFY changed)
public:
    enum Roles {
        TimeRole = Qt::UserRole + 1,
        LevelRole,       // "debug"|"info"|"warning"|"critical"|"fatal"
        LevelSortRole,   // 0..4（着色/过滤用）
        CategoryRole,
        MessageRole
    };

    explicit LogModel(QObject* parent = nullptr);

    // —— 桥（任意线程）——
    static void bridgeAppend(QtMsgType type, const QString& category, const QString& message);

    // —— 模型 ——
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int visibleCount() const { return int(m_rows.size()); }
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString& t);
    bool showDebug() const { return m_showDebug; }
    void setShowDebug(bool v);
    bool showWarning() const { return m_showWarning; }
    void setShowWarning(bool v);
    bool showCritical() const { return m_showCritical; }
    void setShowCritical(bool v);

    Q_INVOKABLE void refresh(); // 立即重算（过滤器变化时由属性 setter 触发）
    Q_INVOKABLE void clear();   // 只清 GUI 展示（不动控制台）

signals:
    void changed();

private:
    void poll();

    struct Row {
        qint64 ms = 0;
        int level = 0; // 0 debug 1 info 2 warning 3 critical 4 fatal
        QString category;
        QString message;
    };
    QList<Row> m_rows;

    QString m_filterText;
    bool m_showDebug = false;
    bool m_showWarning = true;
    bool m_showCritical = true;
    qint64 m_seqSeen = 0; // 上次重算时 ring 的 nextSeq（无新日志时跳过 reset）

    static constexpr int kRingCapacity = 5000;
    static constexpr int kVisibleCap = 3000;

    struct RingEntry {
        qint64 ms;
        int level;
        QString category;
        QString message;
        qint64 seq;
    };
    struct Ring {
        QMutex mutex;
        std::deque<RingEntry> entries;
        qint64 nextSeq = 1;
    };
    static Ring& ring();
};

} // namespace seimi
