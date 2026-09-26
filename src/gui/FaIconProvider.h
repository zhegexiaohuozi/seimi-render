// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QQuickImageProvider>
#include <QSvgRenderer>

namespace seimi {

// SVG 图标提供器（QQuickImageProvider + QSvgRenderer 直连，Qt6::Svg 随主二进制
// 链接——不依赖 imageformats/iconengines 插件，software 渲染后端同样可用）。
//
// 两种模式：
//  1. fa 模式（默认）：id = <name>/<aarrggbb>/<dpr>。加载
//     :/third_party/FontAwesome/<name>.svg（官方原始 SVG，白名单字符校验），
//     按请求尺寸 × DPR 等比光栅化（KeepAspectRatio，FA 各图标 viewBox 比例不一）
//     → SourceIn 单色化（替换 RGB、保留 alpha）。QML 侧见 qml/components/FaIcon.qml。
//  2. 原色模式（构造时给 fixedPath）：id = <dpr>。渲染固定路径的彩色 SVG 原样
//     输出（应用品牌图标 admin-ui/favicon.svg）。QML 侧 Main.qml 品牌区使用。
// requestPixmap 在 pixmap 加载线程执行，本类无可变共享态，天然可重入。
class FaIconProvider : public QQuickImageProvider {
public:
    FaIconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

    explicit FaIconProvider(QString fixedPath)
        : QQuickImageProvider(QQuickImageProvider::Pixmap)
        , m_fixedPath(std::move(fixedPath)) {}

    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override {
        if (!m_fixedPath.isEmpty()) return renderRaw(id, size, requestedSize);

        // id: <name>/<aarrggbb>/<dpr>
        const QStringList seg = id.split(QLatin1Char('/'));
        if (seg.size() < 3) return failPixmap(size);

        const QString name = seg.at(0);
        // 图标名白名单：字母/数字/连字符（FA 命名规则；拒绝 .. 等路径注入）。
        for (const QChar& ch : name) {
            if (!(ch.isLetterOrNumber() || ch == QLatin1Char('-'))) return failPixmap(size);
        }
        if (name.isEmpty() || name.startsWith(QLatin1Char('-'))) return failPixmap(size);

        bool ok = false;
        const uint argb = seg.at(1).toUInt(&ok, 16);
        const QColor tint = ok ? QColor(argb) : QColor(Qt::black);
        qreal dpr = seg.at(2).toDouble();
        if (!(dpr > 0) || dpr > 8) dpr = 1.0;  // 合法性夹取（异常值按 1.0）

        const int target = requestedSize.isValid() && requestedSize.width() > 0
                               ? requestedSize.width()
                               : 22;
        QImage img = rasterize(QStringLiteral(":/third_party/FontAwesome/") + name
                                   + QStringLiteral(".svg"),
                               target, dpr);
        if (img.isNull()) return failPixmap(size);

        // 单色化：源图自身 alpha 作蒙版，RGB 统一替换为 tint。
        QPixmap colored(img.size());
        colored.fill(Qt::transparent);
        QPainter cp(&colored);
        cp.drawPixmap(0, 0, QPixmap::fromImage(img));
        cp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        cp.fillRect(colored.rect(), tint);
        cp.end();

        colored.setDevicePixelRatio(dpr);  // QML 按逻辑尺寸布局（raster/dpr）
        if (size) *size = colored.size();
        return colored;
    }

private:
    QString m_fixedPath;  // 非空 = 原色模式（应用品牌图标）

    // id = <dpr>，渲染 m_fixedPath 原色输出。
    QPixmap renderRaw(const QString& id, QSize* size, const QSize& requestedSize) {
        qreal dpr = id.toDouble();
        if (!(dpr > 0) || dpr > 8) dpr = 1.0;
        const int target = requestedSize.isValid() && requestedSize.width() > 0
                               ? qMax(requestedSize.width(), requestedSize.height())
                               : 64;
        QImage img = rasterize(m_fixedPath, target, dpr);
        if (img.isNull()) return failPixmap(size);
        QPixmap pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(dpr);
        if (size) *size = pm.size();
        return pm;
    }

    // 公共光栅化：等比缩放进 target×dpr 盒，透明底。
    static QImage rasterize(const QString& path, int target, qreal dpr) {
        const int raster = qMax(target, qRound(qreal(target) * dpr));
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return {};
        QSize renderSize = renderer.viewBox().size();
        if (!renderSize.isValid() || renderSize.isEmpty()) renderSize = QSize(raster, raster);
        renderSize.scale(raster, raster, Qt::KeepAspectRatio);
        QImage img(renderSize, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        renderer.render(&p);
        p.end();
        return img;
    }

    static QPixmap failPixmap(QSize* size) {
        if (size) *size = QSize(1, 1);
        QPixmap blank(1, 1);
        blank.fill(Qt::transparent);
        return blank;
    }
};

} // namespace seimi
