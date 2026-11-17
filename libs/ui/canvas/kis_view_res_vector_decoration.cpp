/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_view_res_vector_decoration.h"

#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QScopedArrayPointer>
#include <QVector>
#include <QtMath>

#include <KoColorSpace.h>
#include <KoShapeManager.h>

#include <KisView.h>
#include <kis_canvas2.h>
#include <kis_config.h>
#include <kis_coordinates_converter.h>
#include <kis_display_color_converter.h>
#include <kis_image.h>
#include <kis_paint_device.h>

#include "flake/kis_shape_layer.h"
#include "kis_view_res_vector_compositor.h"

struct KisViewResVectorDecoration::Private {
    KisImageWSP image;

    quint64 signature = 0;
    QVector<KisViewResVectorCompositor::Job> jobs;

    struct JobCache {
        /// document-space content: backdrop + per-unit raster patches
        bool contentValid = false;
        QImage backdrop;
        QVector<QImage> rasterPatches; // parallel to job.units; empty for vector units

        /// view-space content: one crisp patch per vector unit in the job.
        /// Rebuilt only when the linear part of documentToWidgetTransform
        /// or the document-space content changes - panning alone reuses the
        /// patch, it is just drawn at a different widget offset.
        bool viewValid = false;
        /// false when a budget cap prevented building every vector patch;
        /// the job must not be drawn at all then (the canvas pixels already
        /// show the correct, blurry, composite)
        bool vectorComplete = false;
        qreal m11 = 0, m12 = 0, m21 = 0, m22 = 0, dpr = 0;
        QVector<QPair<int, QImage>> vectorPatches; // (unit index in job.units -> patch)
    };

    QVector<JobCache> jobCaches;

    /// union of image-space rects invalidated by sigImageUpdated
    QRect contentDirty;
    QMutex dirtyMutex;
};

const QString KisViewResVectorDecoration::idTag = QStringLiteral("viewResVectorDecoration");

KisViewResVectorDecoration::KisViewResVectorDecoration(QPointer<KisView> parent)
    : KisCanvasDecoration(idTag, parent)
    , m_d(new Private)
{
    // must paint under every UI decoration (grids, guides, assistants,
    // selections) since it replaces part of the image itself
    setPriority(-10);
}

KisViewResVectorDecoration::~KisViewResVectorDecoration()
{
    delete m_d;
}

void KisViewResVectorDecoration::slotImageUpdated(const QRect &rc)
{
    QMutexLocker locker(&m_d->dirtyMutex);
    m_d->contentDirty |= rc;
}

void KisViewResVectorDecoration::drawDecoration(QPainter &gc,
                                                const QRectF &updateArea,
                                                const KisCoordinatesConverter *converter,
                                                KisCanvas2 *canvas)
{
    KisConfig cfg(true);
    if (!cfg.viewResVectorRendering())
        return;

    KisImageSP image = canvas->image();
    if (!image)
        return;

    /**
     * The overlay re-composites the primary image tile only. Wrap-around
     * would need one recomposite per repeated tile; bail instead (documented
     * limitation, rendering stays document-resolution as before).
     * LOD>0 uses reduced-resolution projections where the peel arithmetic
     * on full-resolution unit devices does not apply either.
     */
    if (canvas->wrapAroundViewingMode())
        return;
    if (image->currentLevelOfDetail() > 0)
        return;

    const KisImageSP previousImage = m_d->image;
    if (previousImage != image) {
        if (previousImage) {
            disconnect(previousImage.data(), SIGNAL(sigImageUpdated(QRect)), this, SLOT(slotImageUpdated(QRect)));
        }
        connect(image, SIGNAL(sigImageUpdated(QRect)), SLOT(slotImageUpdated(QRect)), Qt::DirectConnection);
        m_d->image = image;
        m_d->signature = 0;
        m_d->jobs.clear();
        m_d->jobCaches.clear();
        {
            QMutexLocker locker(&m_d->dirtyMutex);
            m_d->contentDirty = image->bounds();
        }
    }

    KisDisplayColorConverter *displayConverter = canvas->displayColorConverter();
    if (!displayConverter)
        return;

    const KisViewResVectorCompositor::DeviceToQImageFn convert = [displayConverter](KisPaintDeviceSP device,
                                                                                    const QRect &rc) -> QImage {
        QScopedArrayPointer<quint8> data(new quint8[size_t(rc.width()) * rc.height() * device->pixelSize()]);
        device->readBytes(data.data(), rc);
        return displayConverter->toQImage(device->colorSpace(), data.data(), rc.size());
    };

    // Rebuild the unit analysis whenever the stack signature changed.
    // The leaf walk is cheap and is also what detects reorder/opacity/
    // visibility changes without any extra signal plumbing.
    QVector<KisViewResVectorCompositor::Unit> units = KisViewResVectorCompositor::collectUnits(image);
    const quint64 signature = KisViewResVectorCompositor::unitsSignature(units);

    if (signature != m_d->signature) {
        m_d->signature = signature;
        m_d->jobs = KisViewResVectorCompositor::computeJobs(units);
        m_d->jobCaches.resize(m_d->jobs.size());
        for (auto &cache : m_d->jobCaches) {
            cache = Private::JobCache();
        }
    }

    /**
     * `units` (the freshly collected list) is used below, never a stored
     * copy: jobCaches only key off the signature and job indices stay valid
     * because an unchanged signature means an identical unit sequence.
     */
    if (m_d->jobs.isEmpty())
        return;

    /**
     * Consume the accumulated dirty region once per repaint and rebuild the
     * document-space caches of every affected job - including jobs that are
     * outside the current update rect, so that a partial repaint never
     * re-enters with a stale cache later.
     */
    QRect dirty;
    {
        QMutexLocker locker(&m_d->dirtyMutex);
        dirty = m_d->contentDirty;
        m_d->contentDirty = QRect();
    }

    const QRect imageWidgetRect = converter->imageRectInWidgetPixels().toAlignedRect();
    const QRect updateWidgetRect = converter->documentToWidget(updateArea).toAlignedRect() & imageWidgetRect;

    const QTransform dtw = converter->documentToWidgetTransform();
    const qreal dpr = converter->devicePixelRatio();

    const qint64 maxJobDevicePixels = 32ll * 1000ll * 1000ll;
    const int maxShapesPerJob = 4096;

    for (int j = 0; j < m_d->jobs.size(); j++) {
        const KisViewResVectorCompositor::Job &job = m_d->jobs[j];
        Private::JobCache &cache = m_d->jobCaches[j];

        if (!cache.contentValid || (!dirty.isEmpty() && job.docRect.intersects(dirty))) {
            cache.backdrop = KisViewResVectorCompositor::buildBackdrop(image, job, units, convert);
            cache.rasterPatches.fill(QImage(), job.units.size());
            for (int i = 0; i < job.units.size(); i++) {
                const auto &unit = units[job.units[i]];
                if (unit.isVector)
                    continue;
                cache.rasterPatches[i] = KisViewResVectorCompositor::buildUnitPatch(unit, job.docRect, convert);
            }
            cache.contentValid = !cache.backdrop.isNull();
            cache.viewValid = false;
        }

        if (!cache.contentValid)
            continue;

        if (!cache.viewValid || cache.m11 != dtw.m11() || cache.m12 != dtw.m12() || cache.m21 != dtw.m21()
            || cache.m22 != dtw.m22() || cache.dpr != dpr) {
            const QSizeF logicalSizeF = converter->imageToWidget(QRectF(job.docRect)).size();
            const QSize logicalSize = QSize(qCeil(logicalSizeF.width()) + 2, qCeil(logicalSizeF.height()) + 2);
            const qint64 devicePixels = qint64(qCeil(logicalSize.width() * dpr)) * qCeil(logicalSize.height() * dpr);

            int numShapes = 0;
            for (int idx : job.units) {
                for (const auto &pair : units[idx].shapeLayers) {
                    numShapes += pair.first->shapeManager()->shapes().size();
                }
            }

            cache.vectorPatches.clear();
            cache.vectorComplete = devicePixels <= maxJobDevicePixels && numShapes <= maxShapesPerJob;
            if (cache.vectorComplete) {
                const QPointF anchor = converter->imageToWidget(QPointF(job.docRect.topLeft()));
                const QTransform patchTransform = QTransform::fromTranslate(-anchor.x(), -anchor.y()) * dtw;

                for (int i = 0; i < job.units.size(); i++) {
                    const auto &unit = units[job.units[i]];
                    if (!unit.isVector)
                        continue;

                    // physical pixels = logical * dpr; the painter then
                    // works in widget-logical coordinates
                    QImage patch(logicalSize * dpr, QImage::Format_ARGB32_Premultiplied);
                    patch.setDevicePixelRatio(dpr);
                    patch.fill(0);

                    QPainter p(&patch);
                    p.setClipRect(QRectF(QPointF(0, 0), QSizeF(logicalSize)));
                    p.setTransform(patchTransform);

                    for (const auto &pair : unit.shapeLayers) {
                        KisShapeLayer *shapeLayer = pair.first;
                        const quint8 childOpacity = pair.second;

                        p.setRenderHint(QPainter::Antialiasing, shapeLayer->antialiased());
                        p.setRenderHint(QPainter::TextAntialiasing, shapeLayer->antialiased());

                        if (childOpacity == 255) {
                            shapeLayer->shapeManager()->paint(p);
                        } else {
                            /**
                             * Group children are composited into the group
                             * buffer at their own effective opacity, so they
                             * must be drawn through an intermediate image
                             * (direct painter opacity would change the
                             * overlap areas of the child's own shapes).
                             */
                            QImage childPatch(logicalSize * dpr, QImage::Format_ARGB32_Premultiplied);
                            childPatch.setDevicePixelRatio(dpr);
                            childPatch.fill(0);
                            QPainter cp(&childPatch);
                            cp.setClipRect(QRectF(QPointF(0, 0), QSizeF(logicalSize)));
                            cp.setTransform(patchTransform);
                            cp.setRenderHint(QPainter::Antialiasing, shapeLayer->antialiased());
                            cp.setRenderHint(QPainter::TextAntialiasing, shapeLayer->antialiased());
                            shapeLayer->shapeManager()->paint(cp);
                            cp.end();
                            p.save();
                            p.setTransform(QTransform());
                            p.setOpacity(childOpacity / 255.0);
                            p.drawImage(QPointF(0, 0), childPatch);
                            p.restore();
                        }
                    }
                    p.end();

                    cache.vectorPatches << qMakePair(i, patch);
                }
            }

            cache.m11 = dtw.m11();
            cache.m12 = dtw.m12();
            cache.m21 = dtw.m21();
            cache.m22 = dtw.m22();
            cache.dpr = dpr;
            cache.viewValid = true;
        }

        if (!cache.vectorComplete)
            continue;

        const QRect jobWidgetRect = converter->imageToWidget(QRectF(job.docRect)).toAlignedRect();
        const QRect clip = jobWidgetRect & updateWidgetRect;
        if (clip.isEmpty())
            continue;

        const QPointF anchor = converter->imageToWidget(QPointF(job.docRect.topLeft()));

        // backdrop: peeled composite of everything outside the job
        gc.save();
        gc.setTransform(QTransform());
        gc.setClipRect(clip, Qt::IntersectClip);
        gc.setTransform(converter->imageToWidgetTransform());
        gc.drawImage(job.docRect.topLeft(), cache.backdrop);
        gc.restore();

        // units in bottom-to-top order: raster units keep their document
        // pixels (identity with the canvas), vector units are re-rendered
        // at view resolution
        for (int i = 0; i < job.units.size(); i++) {
            const auto &unit = units[job.units[i]];
            if (unit.isVector) {
                QImage patch;
                for (const auto &vp : cache.vectorPatches) {
                    if (vp.first == i) {
                        patch = vp.second;
                        break;
                    }
                }
                if (patch.isNull())
                    continue;

                gc.save();
                gc.setTransform(QTransform());
                gc.setClipRect(clip, Qt::IntersectClip);
                gc.setOpacity(unit.opacity / 255.0);
                gc.drawImage(anchor, patch);
                gc.restore();
            } else if (!cache.rasterPatches[i].isNull()) {
                gc.save();
                gc.setTransform(QTransform());
                gc.setClipRect(clip, Qt::IntersectClip);
                gc.setTransform(converter->imageToWidgetTransform());
                gc.drawImage(job.docRect.topLeft(), cache.rasterPatches[i]);
                gc.restore();
            }
        }
    }
}
