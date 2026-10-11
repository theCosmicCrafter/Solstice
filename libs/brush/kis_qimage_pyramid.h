/*
 *  SPDX-FileCopyrightText: 2013 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_QIMAGE_PYRAMID_H
#define __KIS_QIMAGE_PYRAMID_H

#include <QImage>
#include <QVector>
#include <kis_dab_shape.h>
#include <kritabrush_export.h>

// Solstice (GPU engine, "Image tip dabs")
#include <KisImageTipSampler.h>
#include <QSharedPointer>

class BRUSH_EXPORT KisQImagePyramid
{
public:
    KisQImagePyramid() = default;
    KisQImagePyramid(const QImage &baseImage, bool useSmoothingForEnlarging = true);
    ~KisQImagePyramid();

    static QSize imageSize(const QSize &originalSize,
                           KisDabShape const&,
                           qreal subPixelX, qreal subPixelY);

    static QSizeF characteristicSize(const QSize &originalSize, KisDabShape const&);

    /**
     * The transformed tip. Solstice (GPU engine, "Image tip dabs"): drawn by
     * renderDescribed() whenever describeImage() describes the transform, so
     * that the CPU and the GPU brush compute the same pixels on every
     * machine; QPainter (createImageWithQPainter()) only otherwise.
     */
    QImage createImage(KisDabShape const&,
                       qreal subPixelX, qreal subPixelY) const;

    /// The upstream QPainter rendering of createImage() (tests, fallback).
    QImage createImageWithQPainter(KisDabShape const &, qreal subPixelX, qreal subPixelY) const;

    QImage getClosest(QTransform transform, qreal *scale) const;

    QImage getClosestWithoutWorkaroundBorder(QTransform transform, qreal *scale) const;

    /// GPU engine (Solstice, "Image tip dabs"): how createImage() draws a
    /// dab (KisImageTipGeometry, KisImageTipSampler).
    using DabGeometry = KisImageTipGeometry;

    /// False when there is no pyramid or the transform is out of the fixed
    /// point range (createImage() then uses QPainter).
    bool describeImage(KisDabShape const &shape, qreal subPixelX, qreal subPixelY, DabGeometry *geometry) const;

    /// The ARGB32 image createImage() returns for @p geometry, computed from
    /// the description (the reference of the GPU evaluation).
    static QImage renderDescribed(const DabGeometry &geometry);

    /// One pixel of renderDescribed() (dab coordinates).
    static quint32 describedPixel(const DabGeometry &geometry, int x, int y);

private:
    friend class KisGbrBrushTest;
    int findNearestLevel(qreal scale, qreal *baseScale) const;
    void appendPyramidLevel(const QImage &image);

    static void calculateParams(KisDabShape const& shape,
                                qreal subPixelX, qreal subPixelY,
                                const QSize &originalSize,
                                QTransform *outputTransform, QSize *outputSize);

    static void calculateParams(KisDabShape shape,
                                qreal subPixelX, qreal subPixelY,
                                const QSize &originalSize,
                                qreal baseScale, const QSize &baseSize,
                                QTransform *outputTransform, QSize *outputSize);

private:
    QSize m_originalSize;
    qreal m_baseScale {0.0};

    struct PyramidLevel {
        PyramidLevel() {}
        PyramidLevel(QImage _image, QSize _size) : image(_image), size(_size) {}

        QImage image;
        QSize size;
        // Solstice: image as premultiplied QRgba64 (describeImage())
        QSharedPointer<const QVector<quint64>> premultiplied;
    };

    QVector<PyramidLevel> m_levels;
};

#endif /* __KIS_QIMAGE_PYRAMID_H */
