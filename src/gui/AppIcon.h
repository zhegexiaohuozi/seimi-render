// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace seimi {

// 应用图标 = admin-ui/favicon.svg（编入 qrc）：QSvgRenderer 多尺寸光栅化 → QIcon，
// 供窗口 QIcon 与系统托盘使用。不依赖 iconengines/qsvgicon 插件（分发版友好）。
// 首次调用构建后缓存（QIcon 隐式共享）；仅 GUI 线程使用。
inline QIcon appIcon() {
    static QIcon cached;
    if (!cached.isNull()) return cached;
    QSvgRenderer renderer(QStringLiteral(":/admin-ui/favicon.svg"));
    if (!renderer.isValid()) return cached;
    for (int s : {16, 20, 24, 32, 48, 64, 128, 256}) {
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        renderer.render(&p);
        p.end();
        cached.addPixmap(QPixmap::fromImage(img));
    }
    return cached;
}

} // namespace seimi
