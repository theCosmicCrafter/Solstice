/*
 *  SPDX-FileCopyrightText: 2013 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_qimage_pyramid.h"

#include <limits>
#include <QPainter>
#include <kis_debug.h>

// GPU engine (Solstice): describeImage(), renderDescribed()
#include <KisImageTipSampler.h>
#include <cmath>

#define MIPMAP_SIZE_THRESHOLD 512
#define MAX_MIPMAP_SCALE 8.0

#define QPAINTER_WORKAROUND_BORDER 1


KisQImagePyramid::KisQImagePyramid(const QImage &baseImage, bool useSmoothingForEnlarging)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(!baseImage.isNull());

    m_originalSize = baseImage.size();


    qreal scale = MAX_MIPMAP_SCALE;

    while (scale > 1.0) {
        QSize scaledSize = m_originalSize * scale;

        if (scaledSize.width() <= MIPMAP_SIZE_THRESHOLD ||
                scaledSize.height() <= MIPMAP_SIZE_THRESHOLD) {

            if (m_levels.isEmpty()) {
                m_baseScale = scale;
            }

            if (useSmoothingForEnlarging) {
                appendPyramidLevel(baseImage.scaled(scaledSize,  Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            } else {
                appendPyramidLevel(baseImage.scaled(scaledSize,  Qt::IgnoreAspectRatio, Qt::FastTransformation));
            }
        }

        scale *= 0.5;
    }

    if (m_levels.isEmpty()) {
        m_baseScale = 1.0;
    }
    appendPyramidLevel(baseImage);

    scale = 0.5;
    while (true) {
        QSize scaledSize = m_originalSize * scale;

        if (scaledSize.width() == 0 ||
                scaledSize.height() == 0) break;

        appendPyramidLevel(baseImage.scaled(scaledSize,  Qt::IgnoreAspectRatio, Qt::SmoothTransformation));

        scale *= 0.5;
    }
}

KisQImagePyramid::~KisQImagePyramid()
{
}

int KisQImagePyramid::findNearestLevel(qreal scale, qreal *baseScale) const
{
    const qreal scale_epsilon = 1e-6;

    qreal levelScale = m_baseScale;
    int level = 0;
    int lastLevel = m_levels.size() - 1;


    while ((0.5 * levelScale > scale ||
            qAbs(0.5 * levelScale - scale) < scale_epsilon) &&
            level < lastLevel) {

        levelScale *= 0.5;
        level++;
    }

    *baseScale = levelScale;
    return level;
}

inline QRect roundRect(const QRectF &rc)
{
    /**
     * This is an analog of toAlignedRect() with the only difference
     * that it ensures the rect position will never be below zero.
     *
     * Warning: be *very* careful with using bottom()/right() values
     *          of a pure QRect (we don't use it here for the dangers
     *          it can lead to).
     */

    QRectF rect(rc);

    KIS_SAFE_ASSERT_RECOVER_NOOP(rect.x() > -0.000001);
    KIS_SAFE_ASSERT_RECOVER_NOOP(rect.y() > -0.000001);

    if (rect.x() < 0.000001) {
        rect.setLeft(0.0);
    }

    if (rect.y() < 0.000001) {
        rect.setTop(0.0);
    }

    qreal w_rounded = qRound(rect.width());
    qreal h_rounded = qRound(rect.height());

    //Take care of the float precision errors
    if (qAbs(rect.width() - w_rounded) < 0.000001) {
        rect.setWidth(w_rounded);
    }

    if (qAbs(rect.height() - h_rounded) < 0.000001) {
        rect.setHeight(h_rounded);
    }


    return rect.toAlignedRect();
}

QTransform baseBrushTransform(KisDabShape const& shape,
                              qreal subPixelX, qreal subPixelY,
                              const QRectF &baseBounds)
{
    QTransform transform;
    transform.scale(shape.scaleX(), shape.scaleY());

    if (!qFuzzyCompare(shape.rotation(), 0) && !qIsNaN(shape.rotation())) {
        transform = transform * QTransform().rotateRadians(shape.rotation());
        QRectF rotatedBounds = transform.mapRect(baseBounds);
        transform = transform * QTransform::fromTranslate(-rotatedBounds.x(), -rotatedBounds.y());
    }

    return transform * QTransform::fromTranslate(subPixelX, subPixelY);
}

void KisQImagePyramid::calculateParams(KisDabShape const& shape,
                                       qreal subPixelX, qreal subPixelY,
                                       const QSize &originalSize,
                                       QTransform *outputTransform, QSize *outputSize)
{
    calculateParams(shape,
                    subPixelX, subPixelY,
                    originalSize, 1.0, originalSize,
                    outputTransform, outputSize);
}

void KisQImagePyramid::calculateParams(KisDabShape shape,
                                       qreal subPixelX, qreal subPixelY,
                                       const QSize &originalSize,
                                       qreal baseScale, const QSize &baseSize,
                                       QTransform *outputTransform, QSize *outputSize)
{
    Q_UNUSED(baseScale);

    QRectF originalBounds = QRectF(QPointF(), originalSize);
    QTransform originalTransform = baseBrushTransform(shape, subPixelX, subPixelY, originalBounds);

    qreal realBaseScaleX = qreal(baseSize.width()) / originalSize.width();
    qreal realBaseScaleY = qreal(baseSize.height()) / originalSize.height();
    qreal scaleX = shape.scaleX() / realBaseScaleX;
    qreal scaleY = shape.scaleY() / realBaseScaleY;
    shape = KisDabShape(scaleX, scaleY/scaleX, shape.rotation());

    QRectF baseBounds = QRectF(QPointF(), baseSize);
    QTransform transform = baseBrushTransform(shape, subPixelX, subPixelY, baseBounds);
    QRectF mappedRect = originalTransform.mapRect(originalBounds);

    // Set up a 0,0,1,1 size and identity transform in case the transform fails to
    // produce a usable result.
    int width = 1;
    int height = 1;
    *outputTransform = QTransform();

    if (mappedRect.isValid()) {
        QRect expectedDstRect = roundRect(mappedRect);

#if 0 // Only enable when debugging; users shouldn't see this warning
        {
            QRect testingRect = roundRect(transform.mapRect(baseBounds));
            if (testingRect != expectedDstRect) {
                warnKrita << "WARNING: expected and real dab rects do not coincide!";
                warnKrita << "         expected rect:" << expectedDstRect;
                warnKrita << "         real rect:    " << testingRect;
            }
        }
#endif
        KIS_SAFE_ASSERT_RECOVER_NOOP(expectedDstRect.x() >= 0);
        KIS_SAFE_ASSERT_RECOVER_NOOP(expectedDstRect.y() >= 0);

        width = expectedDstRect.x() + expectedDstRect.width();
        height = expectedDstRect.y() + expectedDstRect.height();

        // we should not return invalid image, so adjust the image to be
        // at least 1 px in size.
        width = qMax(1, width);
        height = qMax(1, height);
    }
    else {
#if 0 // Only enable when debugging; users shouldn't see this warning
        qWarning() << "Brush transform generated an invalid rectangle!"
            << ppVar(shape.scaleX()) << ppVar(shape.scaleY()) << ppVar(shape.rotation())
            << ppVar(subPixelX) << ppVar(subPixelY)
            << ppVar(originalSize)
            << ppVar(baseScale)
            << ppVar(baseSize)
            << ppVar(baseBounds)
            << ppVar(mappedRect);
#endif
    }

    *outputTransform = transform;
    *outputSize = QSize(width, height);
}

QSize KisQImagePyramid::imageSize(const QSize &originalSize,
                                  KisDabShape const& shape,
                                  qreal subPixelX, qreal subPixelY)
{
    QTransform transform;
    QSize dstSize;

    calculateParams(shape, subPixelX, subPixelY,
                    originalSize,
                    &transform, &dstSize);

    return dstSize;
}

QSizeF KisQImagePyramid::characteristicSize(const QSize &originalSize,
                                            KisDabShape const& shape)
{
    QRectF originalRect(QPointF(), originalSize);
    QTransform transform = baseBrushTransform(shape,
                                              0.0, 0.0,
                                              originalRect);

    return transform.mapRect(originalRect).size();
}

void KisQImagePyramid::appendPyramidLevel(const QImage &image)
{
    /**
     * QPainter has a bug: when doing a transformation it decides that
     * all the pixels outside of the image (source rect) are equal to
     * the border pixels (CLAMP in terms of openGL). This means that
     * there will be no smooth scaling on the border of the image when
     * it is rotated.  To workaround this bug we need to add one pixel
     * wide border to the image, so that it transforms smoothly.
     *
     * See a unittest in: KisGbrBrushTest::testQPainterTransformationBorder
     */
    
QSize levelSize = image.size();
    QImage tmp = image.convertToFormat(QImage::Format_ARGB32);
    tmp = tmp.copy(-QPAINTER_WORKAROUND_BORDER,
                   -QPAINTER_WORKAROUND_BORDER,
                   image.width() + 2 * QPAINTER_WORKAROUND_BORDER,
                   image.height() + 2 * QPAINTER_WORKAROUND_BORDER);
    PyramidLevel level(tmp, levelSize);
    level.premultiplied = KisImageTipSampler::premultiply(tmp);
    m_levels.append(level);
}

QImage KisQImagePyramid::createImage(KisDabShape const& shape,
                                     qreal subPixelX, qreal subPixelY) const
{
    if (m_levels.isEmpty()) return QImage();

    // GPU engine (Solstice): deterministic, repeated by the GPU brush; see
    // KisImageTipSampler.cpp for why QPainter is no longer used here
    DabGeometry geometry;
    if (describeImage(shape, subPixelX, subPixelY, &geometry)) {
        return renderDescribed(geometry);
    }
    return createImageWithQPainter(shape, subPixelX, subPixelY);
}

QImage KisQImagePyramid::createImageWithQPainter(KisDabShape const &shape, qreal subPixelX, qreal subPixelY) const
{
    if (m_levels.isEmpty())
        return QImage();

    qreal baseScale = -1.0;
    int level = findNearestLevel(shape.scale(), &baseScale);

    const QImage &srcImage = m_levels[level].image;

    QTransform transform;
    QSize dstSize;

    calculateParams(shape, subPixelX, subPixelY,
                    m_originalSize, baseScale, m_levels[level].size,
                    &transform, &dstSize);

    if (transform.isIdentity() &&
            srcImage.format() == QImage::Format_ARGB32) {

        return srcImage.copy(QPAINTER_WORKAROUND_BORDER,
                             QPAINTER_WORKAROUND_BORDER,
                             srcImage.width() - 2 * QPAINTER_WORKAROUND_BORDER,
                             srcImage.height() - 2 * QPAINTER_WORKAROUND_BORDER);
    }

    QImage dstImage(dstSize, QImage::Format_ARGB32);
    dstImage.fill(0);


    /**
     * QPainter has one more bug: when a QTransform is TxTranslate, it
     * does wrong sampling (probably, Nearest Neighbour) even though
     * we tell it directly that we need SmoothPixmapTransform.
     *
     * So here is a workaround: we set a negligible scale to convince
     * Qt we use a non-only-translating transform.
     */
    while (transform.type() == QTransform::TxTranslate) {
        const qreal scale = transform.m11();
        const qreal fakeScale = scale - 10 * std::numeric_limits<qreal>::epsilon();
        transform *= QTransform::fromScale(fakeScale, fakeScale);
    }

    QPainter gc(&dstImage);
    gc.setTransform(
        QTransform::fromTranslate(-QPAINTER_WORKAROUND_BORDER,
                                  -QPAINTER_WORKAROUND_BORDER) * transform);
    gc.setRenderHints(QPainter::SmoothPixmapTransform);
    gc.drawImage(QPointF(), srcImage);
    gc.end();

    return dstImage;
}

QImage KisQImagePyramid::getClosest(QTransform transform, qreal *scale) const
{
    if (m_levels.isEmpty()) return QImage();

    // Estimate scale
    QSizeF transformedUnitSquare = transform.mapRect(QRectF(0, 0, 1, 1)).size();
    qreal x = qAbs(transformedUnitSquare.width());
    qreal y = qAbs(transformedUnitSquare.height());
    qreal estimatedScale = (x > y) ? transformedUnitSquare.width() : transformedUnitSquare.height();

    int level = findNearestLevel(estimatedScale, scale);
    return m_levels[level].image;
}

QImage KisQImagePyramid::getClosestWithoutWorkaroundBorder(QTransform transform, qreal *scale) const
{
    QImage image = getClosest(transform, scale);
    return image.copy(QPAINTER_WORKAROUND_BORDER,
               QPAINTER_WORKAROUND_BORDER,
               image.width() - 2 * QPAINTER_WORKAROUND_BORDER,
               image.height() - 2 * QPAINTER_WORKAROUND_BORDER);
}

namespace
{
constexpr int QtFixedScale = 1 << 16;
constexpr int QtHalfPoint = 1 << 15;
} // namespace

bool KisQImagePyramid::describeImage(KisDabShape const &shape,
                                     qreal subPixelX,
                                     qreal subPixelY,
                                     DabGeometry *geometry) const
{
    if (m_levels.isEmpty() || !geometry)
        return false;

    qreal baseScale = -1.0;
    const int level = findNearestLevel(shape.scale(), &baseScale);
    const QImage &srcImage = m_levels[level].image;

    QTransform transform;
    QSize dstSize;
    calculateParams(shape, subPixelX, subPixelY, m_originalSize, baseScale, m_levels[level].size, &transform, &dstSize);

    *geometry = DabGeometry();
    geometry->source = srcImage;
    geometry->premultiplied = m_levels[level].premultiplied;
    if (srcImage.format() != QImage::Format_ARGB32 || !geometry->premultiplied)
        return false;

    if (transform.isIdentity()) {
        geometry->identity = true;
        geometry->size = QSize(srcImage.width() - 2 * QPAINTER_WORKAROUND_BORDER,
                               srcImage.height() - 2 * QPAINTER_WORKAROUND_BORDER);
        return true;
    }
    geometry->size = dstSize;

    while (transform.type() == QTransform::TxTranslate) {
        const qreal scale = transform.m11();
        const qreal fakeScale = scale - 10 * std::numeric_limits<qreal>::epsilon();
        transform *= QTransform::fromScale(fakeScale, fakeScale);
    }
    const QTransform matrix =
        QTransform::fromTranslate(-QPAINTER_WORKAROUND_BORDER, -QPAINTER_WORKAROUND_BORDER) * transform;
    const bool scaleOnly = matrix.type() == QTransform::TxScale;

    // QRasterPaintEngine::drawImage(const QRectF &r, img, sr) with r = sr
    const QRectF r(0, 0, srcImage.width(), srcImage.height());
    QTransform copy = matrix;
    copy.translate(r.x(), r.y());
    copy.translate(-r.x(), -r.y());

    // QSpanData::setupMatrix()
    QTransform delta;
    delta.translate(1.0 / 65536, 1.0 / 65536);
    const QTransform inv = (delta * copy).inverted();
    const qreal m11 = inv.m11();
    const qreal m12 = inv.m12();
    const qreal m21 = inv.m21();
    const qreal m22 = inv.m22();
    const qreal dx = inv.dx();
    const qreal dy = inv.dy();
    const qreal f1 = m11 * m11 + m21 * m21;
    const qreal f2 = m12 * m12 + m22 * m22;
    const bool fastMatrix = inv.isAffine() && f1 < 1e4 && f2 < 1e4 && f1 > (1.0 / 65536) && f2 > (1.0 / 65536)
        && qAbs(dx) < 1e4 && qAbs(dy) < 1e4;
    if (!fastMatrix)
        return false;

    // Scale only: fillRect_normalized() of the rounded target rect, clipped
    // to the image. Rotated: in every row, the pixels whose centers map into
    // the bordered source rect (like QRasterizer's spans; outside it the
    // samples are the transparent border). Each row starts its 16.16
    // position at its first pixel, as Qt does per span.
    int x1 = 0;
    int y1 = 0;
    int x2 = dstSize.width();
    int y2 = dstSize.height();
    if (scaleOnly) {
        const QRectF rr = matrix.mapRect(r);
        x1 = qMax(qRound(rr.x()), 0);
        y1 = qMax(qRound(rr.y()), 0);
        x2 = qMin(qRound(rr.right()), dstSize.width());
        y2 = qMin(qRound(rr.bottom()), dstSize.height());
    }
    geometry->rows.resize(dstSize.height());
    if (x2 <= x1 || y2 <= y1)
        return true;

    geometry->fdx = int(m11 * QtFixedScale);
    geometry->fdy = int(m12 * QtFixedScale);
    for (int y = y1; y < y2; ++y) {
        const qreal cy = y + qreal(0.5);
        int rowX1 = x1;
        int rowX2 = x2;
        if (!scaleOnly) {
            // centers t = x + 0.5 with 0 <= m11 t + m21 cy + dx <= width and
            // 0 <= m12 t + m22 cy + dy <= height
            qreal tMin = -std::numeric_limits<qreal>::infinity();
            qreal tMax = std::numeric_limits<qreal>::infinity();
            auto limit = [&tMin, &tMax](qreal a, qreal b, qreal hi) {
                if (qAbs(a) < 1e-12) {
                    if (b < 0 || b > hi) {
                        tMin = 1;
                        tMax = 0;
                    }
                    return;
                }
                const qreal t1 = (0 - b) / a;
                const qreal t2 = (hi - b) / a;
                tMin = qMax(tMin, qMin(t1, t2));
                tMax = qMin(tMax, qMax(t1, t2));
            };
            limit(m11, m21 * cy + dx, r.width());
            limit(m12, m22 * cy + dy, r.height());
            if (!(tMin <= tMax))
                continue;
            rowX1 = qMax(x1, int(std::ceil(tMin - 0.5)));
            rowX2 = qMin(x2, int(std::floor(tMax - 0.5)) + 1);
            if (rowX2 <= rowX1)
                continue;
        }
        const int length = rowX2 - rowX1;
        const qreal cx = rowX1 + qreal(0.5);
        // canUseFastMatrixPath()
        qreal fx = (m21 * cy + m11 * cx + dx) * QtFixedScale;
        qreal fy = (m22 * cy + m12 * cx + dy) * QtFixedScale;
        qreal minc = std::min(fx, fy);
        qreal maxc = std::max(fx, fy);
        fx += std::trunc(m11 * QtFixedScale) * length;
        fy += std::trunc(m12 * QtFixedScale) * length;
        minc = std::min(minc, std::min(fx, fy));
        maxc = std::max(maxc, std::max(fx, fy));
        if (!(minc >= std::numeric_limits<int>::min() && maxc <= std::numeric_limits<int>::max()))
            return false;

        DabGeometry::Row &row = geometry->rows[y];
        row.x = rowX1;
        row.length = length;
        row.fx = int((m21 * cy + m11 * cx + dx) * QtFixedScale) - QtHalfPoint;
        row.fy = int((m22 * cy + m12 * cx + dy) * QtFixedScale) - QtHalfPoint;
    }
    return true;
}

quint32 KisQImagePyramid::describedPixel(const DabGeometry &geometry, int x, int y)
{
    return KisImageTipSampler::pixel(geometry, x, y);
}

QImage KisQImagePyramid::renderDescribed(const DabGeometry &geometry)
{
    return KisImageTipSampler::render(geometry);
}
