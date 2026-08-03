// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "HarStore.h"

#include <QDateTime>

#include <algorithm>

namespace seimi {

void HarStore::archive(const QString& taskId, QString harJson) {
    QMutexLocker locker(&m_mutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Entry e;
    e.harJson = std::move(harJson);
    e.archivedAtMsec = now;
    m_entries.insert(taskId, std::move(e));
    sweepLocked(now);
}

HarStore::Result HarStore::get(const QString& taskId) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_entries.constFind(taskId);
    if (it == m_entries.constEnd()) return {false, QString()};
    return {true, it.value().harJson};
}

void HarStore::remove(const QString& taskId) {
    QMutexLocker locker(&m_mutex);
    m_entries.remove(taskId);
}

void HarStore::sweepLocked(qint64 nowMsec) {
    // 1) TTL：过期淘汰。
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if ((nowMsec - it.value().archivedAtMsec) > kHarRetentionMsec) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // 2) 数量上限：超限时按归档时间升序淘汰最旧者。
    if (m_entries.size() > kMaxHarEntries) {
        std::vector<std::pair<qint64, QString>> byAge;
        byAge.reserve(m_entries.size());
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            byAge.emplace_back(it.value().archivedAtMsec, it.key());
        }
        std::sort(byAge.begin(), byAge.end());
        const std::size_t toEvict = static_cast<std::size_t>(m_entries.size() - kMaxHarEntries);
        for (std::size_t i = 0; i < toEvict && i < byAge.size(); ++i) {
            m_entries.remove(byAge[i].second);
        }
    }
}

} // namespace seimi
