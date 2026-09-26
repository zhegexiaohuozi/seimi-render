// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "LogModel.h"

#include <QDateTime>

namespace seimi {

static int levelFromQt(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:    return 0;
        case QtInfoMsg:     return 1;
        case QtWarningMsg:  return 2;
        case QtCriticalMsg: return 3;
        case QtFatalMsg:    return 4;
    }
    return 1;
}

LogModel::Ring& LogModel::ring() {
    static Ring inst;
    return inst;
}

void LogModel::bridgeAppend(QtMsgType type, const QString& category, const QString& message) {
    Ring& r = ring();
    QMutexLocker lock(&r.mutex);
    r.entries.push_back(RingEntry{
        QDateTime::currentMSecsSinceEpoch(), levelFromQt(type), category, message, r.nextSeq++});
    while (int(r.entries.size()) > kRingCapacity) r.entries.pop_front();
}

LogModel::LogModel(QObject* parent)
    : QAbstractListModel(parent)
{
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &LogModel::poll);
    timer->start();
    poll();
}

void LogModel::poll() {
    // 无新日志且过滤器未变：跳过 reset（整表 reset 会打断用户向上翻阅的位置）。
    qint64 seq = 0;
    {
        QMutexLocker lock(&ring().mutex);
        seq = ring().nextSeq;
    }
    if (seq == m_seqSeen) return;
    refresh();
}

void LogModel::refresh() {
    Ring& r = ring();
    QList<Row> rows;
    rows.reserve(kVisibleCap);
    {
        QMutexLocker lock(&r.mutex);
        for (const RingEntry& e : r.entries) {
            if (e.level == 0 && !m_showDebug) continue;          // debug
            if (e.level == 2 && !m_showWarning) continue;        // warning
            if (e.level >= 3 && !m_showCritical) continue;       // critical/fatal
            if (!m_filterText.isEmpty()
                && !e.message.contains(m_filterText, Qt::CaseInsensitive)) {
                continue;
            }
            rows.append(Row{e.ms, e.level, e.category, e.message});
        }
        m_seqSeen = r.nextSeq;
    }
    // 只保留最新 kVisibleCap 条（环形 5000，展示窗口 3000）。
    while (int(rows.size()) > kVisibleCap) rows.removeFirst();

    beginResetModel();
    m_rows = rows;
    endResetModel();
    emit changed();
}

int LogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant LogModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (row < 0 || row >= m_rows.size()) return {};
    const Row& r = m_rows.at(row);
    switch (role) {
        case TimeRole: {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(r.ms);
            return dt.toString(QStringLiteral("HH:mm:ss"));
        }
        case LevelRole: {
            static const char* names[] = {"debug", "info", "warning", "critical", "fatal"};
            return QString::fromLatin1(names[qBound(0, r.level, 4)]);
        }
        case LevelSortRole: return r.level;
        case CategoryRole: return r.category;
        case MessageRole: return r.message;
    }
    return {};
}

QHash<int, QByteArray> LogModel::roleNames() const {
    return {
        {TimeRole, "time"},
        {LevelRole, "level"},
        {LevelSortRole, "levelSort"},
        {CategoryRole, "category"},
        {MessageRole, "message"},
    };
}

void LogModel::setFilterText(const QString& t) {
    if (m_filterText == t) return;
    m_filterText = t;
    refresh();
    emit changed();
}

void LogModel::setShowDebug(bool v) {
    if (m_showDebug == v) return;
    m_showDebug = v;
    refresh();
    emit changed();
}

void LogModel::setShowWarning(bool v) {
    if (m_showWarning == v) return;
    m_showWarning = v;
    refresh();
    emit changed();
}

void LogModel::setShowCritical(bool v) {
    if (m_showCritical == v) return;
    m_showCritical = v;
    refresh();
    emit changed();
}

void LogModel::clear() {
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit changed();
}

} // namespace seimi
