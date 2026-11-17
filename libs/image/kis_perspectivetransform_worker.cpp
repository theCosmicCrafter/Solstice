/*
 * This file is part of Krita
 *
 *  SPDX-FileCopyrightText: 2006 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2009 Edward Apap <schumifer@hotmail.com>
 *  SPDX-FileCopyrightText: 2010 Marc Pegon <pe.marc@free.fr>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#include "kis_perspectivetransform_worker.h"

#include <QMatrix4x4>
#include <QTransform>
#include <QVector3D>
#include <QPolygonF>

#include <KoUpdater.h>
#include <KoColor.h>
#include <KoCompositeOpRegistry.h>

#include "gpu/KisGpuPerspectiveWorker.h"
#include "kis_algebra_2d.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_painter.h"
#include "kis_perspective_math.h"
#include "kis_progress_update_helper.h"
#include "kis_random_accessor_ng.h"
#include "kis_random_sub_accessor.h"
#include "kis_selection.h"
#include "krita_utils.h"
#include <kis_iterator_ng.h>

#include <QPair>
#include <type_traits>

KisPerspectiveTransformWorker::KisPerspectiveTransformWorker(KisPaintDeviceSP dev, QPointF center, double aX, double aY, double distance, bool cropDst, KoUpdaterPtr progress)
        : m_dev(dev), m_progressUpdater(progress), m_cropDst(cropDst)

{
    QMatrix4x4 m;
    m.rotate(180. * aX / M_PI, QVector3D(1, 0, 0));
    m.rotate(180. * aY / M_PI, QVector3D(0, 1, 0));

    QTransform project = m.toTransform(distance);
    QTransform t = QTransform::fromTranslate(center.x(), center.y());

    QTransform forwardTransform = t.inverted() * project * t;

    init(forwardTransform);
}

KisPerspectiveTransformWorker::KisPerspectiveTransformWorker(KisPaintDeviceSP dev, const QTransform &transform, bool cropDst, KoUpdaterPtr progress)
    : m_dev(dev), m_progressUpdater(progress), m_cropDst(cropDst)
{
    init(transform);
}

void KisPerspectiveTransformWorker::fillParams(const QRectF &srcRect,
                                               const QRect &dstBaseClipRect,
                                               KisRegion *dstRegion,
                                               QPolygonF *dstClipPolygon)
{
    QPolygonF bounds = srcRect;
    QPolygonF newBounds = m_forwardTransform.map(bounds);

    QRectF clipRect = dstBaseClipRect;

    if (!m_cropDst) {
        clipRect |= srcRect;
        clipRect = KisAlgebra2D::blowRect(clipRect, 3.0);
    }

    newBounds = newBounds.intersected(clipRect);
    QPainterPath path;
    path.addPolygon(newBounds);
    *dstRegion = KritaUtils::splitPath(path);
    *dstClipPolygon = newBounds;
}

void KisPerspectiveTransformWorker::init(const QTransform &transform)
{
    m_isIdentity = transform.isIdentity();
    m_isTranslating = transform.type() == QTransform::TxTranslate;

    m_forwardTransform = transform;
    m_backwardTransform = transform.inverted();

    if (m_dev) {
        m_srcRect = kisGrowRect(m_dev->exactBounds(), 1.0);

        QPolygonF dstClipPolygonUnused;

        fillParams(m_srcRect,
                   m_dev->defaultBounds()->bounds(),
                   &m_dstRegion,
                   &dstClipPolygonUnused);
    }
}

KisPerspectiveTransformWorker::~KisPerspectiveTransformWorker()
{
}

void KisPerspectiveTransformWorker::setForwardTransform(const QTransform &transform)
{
    init(transform);
}


struct BilinearWrapper
{
    using SrcAccessorSP = KisRandomSubAccessorSP;

    BilinearWrapper(KisPaintDeviceSP device)
        : m_accessor(device->createRandomSubAccessor())
    {
    }

    void samplePixel(const QPointF &pt, quint8 *dst) {
        m_accessor->moveTo(pt.x(), pt.y());
        m_accessor->sampledOldRawData(dst);
    }

    KisRandomSubAccessorSP m_accessor;
};

struct NearestNeighbourWrapper
{
    using SrcAccessorSP = KisRandomAccessorSP;

    NearestNeighbourWrapper(KisPaintDeviceSP device)
        : m_accessor(device->createRandomConstAccessorNG()),
          m_pixelSize(device->pixelSize())
    {
    }

    void samplePixel(const QPointF &pt, quint8 *dst) {
        m_accessor->moveTo(qRound(pt.x()), qRound(pt.y()));
        memcpy(dst, m_accessor->oldRawData(), m_pixelSize);
    }

    KisRandomConstAccessorSP m_accessor;
    int m_pixelSize;
};

namespace
{
/**
 * GPU engine (Solstice): flattens the rect list the CPU iterates (the
 * destination region's rects, or a single dstRect) into the per-row span
 * tables of KisGpuPerspectiveWorker::run(). Every row gets the row's
 * [xStart, xEnd) intervals; a row may hold several of them. Returns false
 * for a pathological region size (the caller then runs the CPU loops).
 */
bool buildRegionSpans(const QVector<QRect> &rects, KisGpuPerspectiveWorker::Region *region)
{
    QRect bounds;
    for (const QRect &rect : rects) {
        bounds |= rect;
    }
    if (bounds.isEmpty()) {
        region->bounds = bounds;
        return true;
    }

    // Reject before allocating the per-row table: a tall region with few
    // spans would otherwise grow far past the span cap intends.
    if (qint64(bounds.height()) > (qint64(1) << 22)) {
        return false;
    }

    QVector<QVector<QPair<qint32, qint32>>> perRow(bounds.height());
    qint64 spanCount = 0;
    for (const QRect &rect : rects) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            perRow[y - bounds.top()].append(qMakePair(rect.left(), rect.right() + 1));
            // 4M spans is far beyond any region this worker produces.
            if (++spanCount > (qint64(1) << 22)) {
                return false;
            }
        }
    }

    region->bounds = bounds;
    region->rows.resize(bounds.height() * 2);
    region->spans.reserve(int(spanCount) * 2);
    for (int i = 0; i < bounds.height(); ++i) {
        region->rows[2 * i] = region->spans.size() / 2;
        region->rows[2 * i + 1] = perRow[i].size();
        for (const QPair<qint32, qint32> &span : perRow[i]) {
            region->spans.append(span.first);
            region->spans.append(span.second);
        }
    }
    return true;
}
} // namespace

template<class SrcAccessorWrapper>
void KisPerspectiveTransformWorker::runImpl()
{
    KIS_ASSERT_RECOVER_RETURN(m_dev);

    if (m_isIdentity) return;

    // TODO: check if this optimization is possible. The only blocking issue might be if
    //       some other thread also accesses this device (which should not be the case,
    //       theoretically
    //
    // if (m_isTranslating) {
    //     m_dev->moveTo(m_dev->offset() + QPoint(qRound(m_forwardTransform.dx()), qRound(m_forwardTransform.dy())));
    //     return;
    // }

    KisPaintDeviceSP cloneDevice = new KisPaintDevice(*m_dev.data());

    // Clear the destination device, since all the tiles are already
    // shared with cloneDevice
    m_dev->clear();

    KIS_ASSERT_RECOVER_NOOP(!m_isIdentity);

    KisProgressUpdateHelper progressHelper(m_progressUpdater, 100, m_dstRegion.rectCount());

    // GPU engine (Solstice): the whole backward map in one submission;
    // the progress helper's destructor reports the completed portion.
    {
        KisGpuPerspectiveWorker::Region region;
        const bool nearest = !std::is_same<typename SrcAccessorWrapper::SrcAccessorSP, KisRandomSubAccessorSP>::value;
        if (buildRegionSpans(m_dstRegion.rects(), &region)
            && KisGpuPerspectiveWorker::run(cloneDevice,
                                            m_dev,
                                            m_backwardTransform,
                                            m_srcRect,
                                            region,
                                            KisGpuPerspectiveWorker::OverwriteAll,
                                            nearest)) {
            return;
        }
    }

    SrcAccessorWrapper srcAcc(cloneDevice);
    KisRandomAccessorSP accessor = m_dev->createRandomAccessorNG();

    Q_FOREACH (const QRect &rect, m_dstRegion.rects()) {
        for (int y = rect.y(); y < rect.y() + rect.height(); ++y) {
            for (int x = rect.x(); x < rect.x() + rect.width(); ++x) {

                QPointF dstPoint(x, y);
                QPointF srcPoint = m_backwardTransform.map(dstPoint);

                if (m_srcRect.contains(srcPoint)) {
                    accessor->moveTo(dstPoint.x(), dstPoint.y());
                    srcAcc.samplePixel(srcPoint, accessor->rawData());
                }
            }
        }
        progressHelper.step();
    }
}

void KisPerspectiveTransformWorker::run(SampleType sampleType)
{
    if (sampleType == Bilinear) {
        runImpl<BilinearWrapper>();
    } else {
        runImpl<NearestNeighbourWrapper>();
    }
}

void KisPerspectiveTransformWorker::runPartialDst(KisPaintDeviceSP srcDev,
                                                  KisPaintDeviceSP dstDev,
                                                  const QRect &dstRect)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(srcDev->pixelSize() == dstDev->pixelSize());
    KIS_SAFE_ASSERT_RECOVER_NOOP(*srcDev->colorSpace() == *dstDev->colorSpace());

    QRectF srcClipRect = kisGrowRect(srcDev->exactBounds(), 1) | srcDev->defaultBounds()->imageBorderRect();
    if (srcClipRect.isEmpty()) return;

    if (m_isIdentity || (m_isTranslating && !m_forceSubPixelTranslation)) {
        KisPainter gc(dstDev);
        gc.setCompositeOpId(COMPOSITE_COPY);
        gc.bitBlt(dstRect.topLeft(), srcDev, m_backwardTransform.mapRect(dstRect));
    } else {
        KisProgressUpdateHelper progressHelper(m_progressUpdater, 100, dstRect.height());

        // GPU engine (Solstice): the chunk's whole backward map in one
        // submission; the helper's destruction reports the chunk done.
        {
            KisGpuPerspectiveWorker::Region region;
            if (buildRegionSpans({dstRect}, &region)
                && KisGpuPerspectiveWorker::run(srcDev,
                                                dstDev,
                                                m_backwardTransform,
                                                srcClipRect,
                                                region,
                                                KisGpuPerspectiveWorker::KeepUnwritten,
                                                false)) {
                return;
            }
        }

        KisRandomSubAccessorSP srcAcc = srcDev->createRandomSubAccessor();
        KisRandomAccessorSP accessor = dstDev->createRandomAccessorNG();

        for (int y = dstRect.y(); y < dstRect.y() + dstRect.height(); ++y) {
            for (int x = dstRect.x(); x < dstRect.x() + dstRect.width(); ++x) {

                QPointF dstPoint(x, y);
                QPointF srcPoint = m_backwardTransform.map(dstPoint);

                if (srcClipRect.contains(srcPoint) || srcDev->defaultBounds()->wrapAroundMode()) {
                    accessor->moveTo(dstPoint.x(), dstPoint.y());
                    srcAcc->moveTo(srcPoint.x(), srcPoint.y());
                    srcAcc->sampledOldRawData(accessor->rawData());
                }
            }
            progressHelper.step();
        }
    }
}

QTransform KisPerspectiveTransformWorker::forwardTransform() const
{
    return m_forwardTransform;
}

QTransform KisPerspectiveTransformWorker::backwardTransform() const
{
    return m_backwardTransform;
}

bool KisPerspectiveTransformWorker::forceSubPixelTranslation() const
{
    return m_forceSubPixelTranslation;
}

void KisPerspectiveTransformWorker::setForceSubPixelTranslation(bool value)
{
    m_forceSubPixelTranslation = value;
}
