// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <utility>

namespace seimi {

// HAR 录制产物归档存储。
//
// 设计：独立的线程安全中枢（仿 CookieStore/ProxyConfig 模式），不污染 RenderTask/
// RenderResult/Snapshot 的瘦结构——HAR 体积可能达数百 KB 甚至数 MB，进 task 表会撑爆
// kMaxLiveTasks=1000 的内存边界。
//
// 线程：GUI 线程 archive（录制完成归档），HTTP 线程 get（查询下载）。
// 淘汰：TTL + 数量上限（LRU），防无限堆积。
class HarStore {
public:
    HarStore() = default;

    // 归档一个 task 的 HAR JSON。
    void archive(const QString& taskId, QString harJson);

    // 取一个 task 的 HAR JSON。found=false 表示不存在/已淘汰。
    struct Result {
        bool found = false;
        QString harJson;
    };
    Result get(const QString& taskId) const;

    // 主动移除（如 task 被淘汰时联动清理）。
    void remove(const QString& taskId);

private:
    void sweepLocked(qint64 nowMsec);

    struct Entry {
        QString harJson;
        qint64 archivedAtMsec = 0;
    };

    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;

    // 保留策略（代码常量，不进 CLI——不够通用）。
    static constexpr qint64 kHarRetentionMsec = 10 * 60 * 1000;   // 10 分钟
    static constexpr int kMaxHarEntries = 200;                      // 最多保留 200 份 HAR
};

} // namespace seimi
