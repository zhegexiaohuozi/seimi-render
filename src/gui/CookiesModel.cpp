// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "CookiesModel.h"

#include <QTimer>

namespace seimi {

CookiesModel::CookiesModel(CookieStore* store, QObject* parent)
    : QAbstractListModel(parent)
    , m_store(store)
{
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &CookiesModel::poll);
    timer->start();
    poll();
}

void CookiesModel::poll() {
    if (!m_store) return;
    const CookieStore::Overview ov = m_store->snapshot(500);
    if (ov.total == m_knownTotal && int(ov.domains.size()) == m_rows.size()) {
        return; // 无变化，不打扰 QML
    }
    m_knownTotal = ov.total;
    refresh();
}

void CookiesModel::refresh() {
    if (!m_store) return;
    const CookieStore::Overview ov = m_store->snapshot(500);
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(int(ov.domains.size()));
    for (const auto& d : ov.domains) {
        m_rows.append(Row{QString::fromStdString(d.domain), d.count});
    }
    m_total = ov.total;
    m_knownTotal = ov.total;
    endResetModel();
    emit changed();
}

int CookiesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant CookiesModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (row < 0 || row >= m_rows.size()) return {};
    switch (role) {
        case DomainRole: return m_rows.at(row).domain;
        case CountRole:  return m_rows.at(row).count;
    }
    return {};
}

QHash<int, QByteArray> CookiesModel::roleNames() const {
    return {
        {DomainRole, "domain"},
        {CountRole, "count"},
    };
}

void CookiesModel::clearSession() {
    if (m_store) m_store->requestClear();
    refresh();
}

void CookiesModel::purgePermanent() {
    if (m_store) m_store->requestPurge();
    refresh();
}

void CookiesModel::flushStore() {
    if (m_store) m_store->flush();
}

} // namespace seimi
