/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2004-2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Clarence Dang <dang@kde.org>
 *  SPDX-FileCopyrightText: 2004 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2004 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_brushop.h"

#include <atomic>
#include "KisPaintTrace.h"

#include <QRect>

#include <kis_image.h>
#include <kis_vec.h>
#include <kis_debug.h>

#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorTransformation.h>

#include <kis_brush.h>
#include <kis_global.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_brush_based_paintop_settings.h>
#include <kis_lod_transform.h>
#include <kis_paintop_plugin_utils.h>
#include "krita_utils.h"
#include "kis_algebra_2d.h"
#include <KisDabRenderingExecutor.h>
#include <KisDabCacheUtils.h>
#include <KisRenderedDab.h>
#include <kis_tool_freehand.h>
#include "KisBrushOpResources.h"

#include <KisRunnableStrokeJobData.h>
#include <KisRunnableStrokeJobUtils.h>
#include <KisRunnableStrokeJobsInterface.h>

#include "gpu/KisGpuBrushPainter.h"
#include "gpu/KisGpuMaskingWorker.h"
#include "kis_image_config.h"
#include "kis_wrapped_rect.h"
#include <QAtomicInt>
#include <QThread>

KisBrushOp::KisBrushOp(const KisPaintOpSettingsSP settings, KisPainter *painter, KisNodeSP node, KisImageSP image)
    : KisBrushBasedPaintOp(settings, painter, SupportsGradientMode | SupportsLightnessMode)
    , m_sizeOption(settings.data())
    , m_ratioOption(settings.data())
    , m_rateOption(settings.data())
    , m_softnessOption(settings.data())
    , m_lightnessStrengthOption(settings.data())
    , m_spacingOption(settings.data())
    , m_scatterOption(settings.data())
    , m_sharpnessOption(settings.data())
    , m_rotationOption(settings.data())
    , m_opacityOption(settings.data(), node)
    , m_avgSpacing(50)
    , m_avgNumDabs(50)
    , m_avgUpdateTimePerDab(50)
    , m_idealNumRects(KisImageConfig(true).maxNumberOfThreads())
    , m_minUpdatePeriod(cpuMinimumUpdatePeriod())
    , m_maxUpdatePeriod(100)
{
    // Only float images use the GPU-aware merge scheduling. A float layer
    // inside an integer image must keep the CPU brush path too.
    m_isRgbaFloatImage = image && image->colorSpace()->colorModelId() == RGBAColorModelID
        && (image->colorSpace()->colorDepthId() == Float32BitsColorDepthID
            || image->colorSpace()->colorDepthId() == Float16BitsColorDepthID);
    Q_ASSERT(settings);
    // Solstice (2026-10-10): a stroke with a masking brush uses the GPU
    // brush only when its masking composite runs on the GPU too
    // (KisGpuMaskingWorker; the stroke decides the same way). A CPU
    // composite over GPU-painted tiles read them back and uploaded them again
    // for every update: up to 2.4 times slower than the CPU brush
    // (docs/agent/gpu-work-priorities.md).
    m_maskedStroke = settings->hasMaskingSettings();
    if (m_maskedStroke
        && !(painter && painter->device()
             && KisGpuMaskingWorker::supports(painter->device()->colorSpace(), settings->maskingBrushCompositeOp()))) {
        m_isRgbaFloatImage = false;
    }
    // The masking brush of such a stroke paints into an alpha mask, which the
    // GPU brush does not support either
    if (painter && painter->device() && painter->device()->colorSpace()->colorModelId() != RGBAColorModelID) {
        m_isRgbaFloatImage = false;
    }

    m_airbrushData.read(settings.data());
    m_rotationOption.applyFanCornersInfo(this);

    m_precisionOption.setHasImprecisePositionOptions(m_precisionOption.hasImprecisePositionOptions()
                                                     || m_scatterOption.isChecked() || m_rotationOption.isChecked()
                                                     || m_airbrushData.isChecked);

    m_brush->notifyBrushIsGoingToBeClonedForStroke();

    KisBrushSP baseBrush = m_brush;
    // decided once per stroke: reading the environment for every dab
    // contended a global lock across the dab threads
    const bool gpuDabs = m_isRgbaFloatImage && KisGpuBrushPainter::isEnabled();
    auto resourcesFactory =
        [baseBrush, settings, painter, gpuDabs] () {
            KisBrushOpResources *resources = new KisBrushOpResources(settings, painter);
            resources->gpuDabs = gpuDabs;
            resources->brush = baseBrush->clone().dynamicCast<KisBrush>();

            return resources;
        };


    m_dabExecutor.reset(
        new KisDabRenderingExecutor(
                    painter->device()->compositionSourceColorSpace(),
                    resourcesFactory,
                    painter->runnableStrokeJobsInterface(),
                    &m_mirrorOption,
                    &m_precisionOption));
}

KisBrushOp::~KisBrushOp()
{
}

KisSpacingInformation KisBrushOp::paintAt(const KisPaintInformation& info)
{
    if (!painter()->device()) return KisSpacingInformation(1.0);

    KisBrushSP brush = m_brush;
    Q_ASSERT(brush);
    if (!brush)
        return KisSpacingInformation(1.0);

    if (!brush->canPaintFor(info))
        return KisSpacingInformation(1.0);

    qreal scale = applyMinimumDabSize(m_sizeOption.apply(info));
    scale *= KisLodTransform::lodToScale(painter()->device());
    if (checkSizeTooSmall(scale)) return KisSpacingInformation();

    qreal rotation = m_rotationOption.apply(info);
    qreal ratio = m_ratioOption.apply(info);

    KisDabShape shape(scale, ratio, rotation);
    QPointF cursorPos =
        m_scatterOption.apply(info,
                              brush->maskWidth(shape, 0, 0, info),
                              brush->maskHeight(shape, 0, 0, info));

    qreal dabOpacity = OPACITY_OPAQUE_F;
    qreal dabFlow = OPACITY_OPAQUE_F;

    m_opacityOption.apply(info, &dabOpacity, &dabFlow);

    KisDabCacheUtils::DabRequestInfo request(painter()->paintColor(),
                                             cursorPos,
                                             shape,
                                             info,
                                             m_softnessOption.apply(info),
                                             m_lightnessStrengthOption.apply(info));

    m_dabExecutor->addDab(request, dabOpacity, dabFlow);


    KisSpacingInformation spacingInfo =
        effectiveSpacing(scale, rotation, &m_airbrushData, &m_spacingOption, info);

    // gather statistics about dabs
    m_avgSpacing(spacingInfo.scalarApprox());

    return spacingInfo;
}

namespace
{
std::atomic<quint64> s_materializedDabs{0};

int initialGpuUpdatePeriod()
{
    bool valid = false;
    const int value = qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_MIN_UPDATE_MS", &valid);
    return valid ? qBound(-1, value, 100) : -1;
}
std::atomic<int> s_gpuUpdatePeriod{initialGpuUpdatePeriod()};

void materializePendingDabs(QList<KisRenderedDab> &dabs)
{
    for (KisRenderedDab &dab : dabs) {
        if (dab.pixelsPending) {
            dab.materialize();
            s_materializedDabs++;
        }
    }
}
} // namespace

quint64 KisBrushOp::materializedDabCount()
{
    return s_materializedDabs.load();
}

int KisBrushOp::cpuMinimumUpdatePeriod()
{
    // Solstice (input to display, 2026-10-10): the CPU path waited at least
    // 10 ms before each batch although a batch of a light brush paints in
    // about 1 ms, so dabs waited about 10 ms. The period still adapts to the
    // measured rendering time (1.5 times it, up to 100 ms), which holds heavy
    // brushes back. KRITA_CPU_BRUSH_MIN_UPDATE_MS overrides it for
    // comparisons; docs/agent/gpu-work-priorities.md.
    bool valid = false;
    const int value = qEnvironmentVariableIntValue("KRITA_CPU_BRUSH_MIN_UPDATE_MS", &valid);
    return valid ? qBound(0, value, 100) : 0;
}

int KisBrushOp::gpuMinimumUpdatePeriod()
{
    // GPU engine (Solstice, phase 4.88): -1 lets the stroke start a batch at
    // every trigger (input or finished dab job) once the previous batch is
    // done. KRITA_GPU_BRUSH_MIN_UPDATE_MS overrides it for comparisons.
    return s_gpuUpdatePeriod.load();
}

void KisBrushOp::setGpuMinimumUpdatePeriodForTesting(int period)
{
    s_gpuUpdatePeriod = period < -1 ? initialGpuUpdatePeriod() : qMin(period, 100);
}

struct KisBrushOp::UpdateSharedState {
    // rendering data
    KisPainter *painter = 0;
    QList<KisRenderedDab> dabsQueue;
    bool gpuMirrorsPainted = false; // published by a sequential job before reflection jobs
    quint64 paintTraceBatch = 0;

    // speed metrics
    QVector<QPointF> dabPoints;
    QElapsedTimer dabRenderingTimer;

    // final report
    QVector<QRect> allDirtyRects;
};

void KisBrushOp::addMirroringJobs(Qt::Orientation direction,
                                  QVector<QRect> &rects,
                                  UpdateSharedStateSP state,
                                  QVector<KisRunnableStrokeJobData*> &jobs)
{
    KritaUtils::addJobSequential(jobs, nullptr);

    /**
     * Some KisRenderedDab may share their devices, so we should mirror them
     * carefully, avoiding doing that twice. KisDabRenderingQueue is implemented in
     * a way that duplicated dabs can go only sequentially, one after another, so
     * we don't have to use complex deduplication algorithms here.
     */
    KisFixedPaintDeviceSP prevDabDevice = 0;
    for (KisRenderedDab &dab : state->dabsQueue) {
        const bool skipMirrorPixels = prevDabDevice && prevDabDevice == dab.device;

        KritaUtils::addJobConcurrent(jobs, [state, &dab, direction, skipMirrorPixels]() {
            if (!state->gpuMirrorsPainted) {
                state->painter->mirrorDab(direction, &dab, skipMirrorPixels);
                // The pixels are mirrored, by this dab or by the previous one
                // sharing its device; keep the GPU description in step.
                dab.proceduralFlips ^= direction == Qt::Horizontal ? 1u : 2u;
            }
        });

        prevDabDevice = dab.device;
    }

    KritaUtils::addJobSequential(jobs, nullptr);

    for (QRect &rc : rects) {
        state->painter->mirrorRect(direction, &rc);
    }
    addDabPaintingJobs(rects, state, jobs, true);

    state->allDirtyRects.append(rects);
}

void KisBrushOp::addDabPaintingJobs(const QVector<QRect> &rects,
                                    UpdateSharedStateSP state,
                                    QVector<KisRunnableStrokeJobData *> &jobs,
                                    bool mirroredPass)
{
    if (m_isRgbaFloatImage && KisGpuBrushPainter::supports(state->painter)) {
        // GPU tile writes must not be split into parallel pixel rectangles:
        // disjoint rectangles can still share one 64x64 destination tile.
        // For mirror passes, the preceding sequential barrier also ensures
        // that all source pixels and positions have finished reflecting.
        KritaUtils::addJobSequential(jobs, [state, rects, mirroredPass]() {
            KisPaintTrace::Scope trace("brush.gpu_attempt_and_fallback", state->painter);
            KisPaintTrace::link("batch.paint_job", state->painter, KisPaintTrace::currentJob(), state->paintTraceBatch);
            if (state->gpuMirrorsPainted)
                return;
            if (!mirroredPass && state->painter->hasMirroring()) {
                state->gpuMirrorsPainted = KisGpuBrushPainter::paintMirrored(state->painter, state->dabsQueue, rects);
                if (state->gpuMirrorsPainted)
                    return;
            }
            const auto *paintRects = state->painter->hasMirroring() ? &rects : nullptr;
            if (!KisGpuBrushPainter::paint(state->painter, state->dabsQueue, paintRects)) {
                // Dabs whose CPU generation was skipped get their pixels now.
                materializePendingDabs(state->dabsQueue);
                for (const QRect &rc : rects)
                    state->painter->bltFixed(rc, state->dabsQueue);
                KisPaintTrace::link("path.brush.cpu_fallback", state->painter, KisPaintTrace::currentJob());
            } else if (mirroredPass && qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1) {
                static QAtomicInt messages{0};
                if (messages.fetchAndAddRelaxed(1) < 12)
                    qInfo() << "GPU brush: reflected pass mode" << state->painter->compositeOpId() << "dabs"
                            << state->dabsQueue.size();
            }
        });
    } else {
        if (KisRenderedDab::hasPendingPixels(state->dabsQueue)) {
            // E.g. LOD or another unsupported GPU case after the dabs were described.
            KritaUtils::addJobSequential(jobs, [state]() {
                materializePendingDabs(state->dabsQueue);
            });
        }
        Q_FOREACH (const QRect &rc, rects) {
            KritaUtils::addJobConcurrent(jobs, [rc, state]() {
                KisPaintTrace::Scope trace("brush.cpu_composite", state->painter);
                KisPaintTrace::link("batch.paint_job",
                                    state->painter,
                                    KisPaintTrace::currentJob(),
                                    state->paintTraceBatch);
                if (!state->gpuMirrorsPainted) {
                    state->painter->bltFixed(rc, state->dabsQueue);
                    KisPaintTrace::link("path.brush.cpu", state->painter, KisPaintTrace::currentJob());
                }
            });
        }
    }
}

std::pair<int, bool> KisBrushOp::doAsynchronousUpdate(QVector<KisRunnableStrokeJobData *> &jobs)
{
    bool someDabsAreStillInQueue = false;
    bool stoppedByByteLimit = false;
    const bool hasPreparedDabsAtStart = m_dabExecutor->hasPreparedDabs();

    // GPU engine (Solstice, phase 4.88): on the GPU path a batch costs a
    // submission, not CPU rasterization by worker threads, so the CPU-tuned
    // 10-100 ms update period only delays dabs that are already ready.
    // A masked stroke keeps the adaptive period: each batch also composites
    // the mask and merges the projection, so batches of the few dabs that
    // CPU-generated (e.g. textured) dabs make ready at a time cost more than
    // they save (docs/agent/gpu-engine.md, "Masking brush").
    const bool gpuPath = m_isRgbaFloatImage && KisGpuBrushPainter::supports(painter()) && !m_maskedStroke;
    if (gpuPath) {
        m_currentUpdatePeriod = gpuMinimumUpdatePeriod();
    }

    if (!m_updateSharedState && hasPreparedDabsAtStart) {
        m_updateSharedState = toQShared(new UpdateSharedState());
        UpdateSharedStateSP state = m_updateSharedState;

        state->painter = painter();
        state->paintTraceBatch = KisPaintTrace::nextId();

        {
            const qreal dabRenderingTime = m_dabExecutor->averageDabRenderingTime();
            const qreal totalRenderingTimePerDab = dabRenderingTime + m_avgUpdateTimePerDab.rollingMeanSafe();

            // we limit the number of fetched dabs to fit the maximum update period and not
            // make visual hiccups
            const int dabsLimit =
                totalRenderingTimePerDab > 0 ?
                    qMax(10, int(m_maxUpdatePeriod  / totalRenderingTimePerDab * m_idealNumRects)) :
                    -1;

            // Bound actual source bytes, not just the moving average of dab
            // execution time. Leave room in the 64 MiB GPU staging budget for
            // destination tables, clipped records and selection coverage.
            const quint64 byteLimit =
                m_isRgbaFloatImage && KisGpuBrushPainter::supports(painter()) ? quint64(32) << 20 : ~quint64(0);
            state->dabsQueue = m_dabExecutor->takeReadyDabs(painter()->hasMirroring(),
                                                            dabsLimit,
                                                            &someDabsAreStillInQueue,
                                                            byteLimit,
                                                            state->paintTraceBatch,
                                                            &stoppedByByteLimit);
        }

        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(!state->dabsQueue.isEmpty(),
                                             std::make_pair(m_currentUpdatePeriod, false));
        KisPaintTrace::link("batch.ready", painter(), state->paintTraceBatch, KisPaintTrace::currentJob());
        if (stoppedByByteLimit) {
            KisPaintTrace::link("batch.byte_limited", painter(), state->paintTraceBatch);
        }

        const int diameter = m_dabExecutor->averageDabSize();
        const qreal spacing = m_avgSpacing.rollingMean();

        const int idealNumRects = m_idealNumRects;

        QVector<QRect> rects;

        // wrap the dabs if needed
        if (painter()->device()->defaultBounds()->wrapAroundMode()) {
            /**
             * In WA mode we do two things:
             *
             * 1) We ensure that the parallel threads do not access the same are on
             *    the image. For normal updates that is ensured by the code in KisImage
             *    and the scheduler. Here we should do that manually by adjusting 'rects'
             *    so that they would not intersect in the wrapped space.
             *
             * 2) We duplicate dabs, to ensure that all the pieces of dabs are painted
             *    inside the wrapped rect. No pieces are dabs are painted twice, because
             *    we paint only the parts intersecting the wrap rect.
             */

            const QRect wrapRect = painter()->device()->defaultBounds()->imageBorderRect();
            const WrapAroundAxis wrapAroundModeAxis = painter()->device()->defaultBounds()->wrapAroundModeAxis();

            QList<KisRenderedDab> wrappedDabs;

            Q_FOREACH (const KisRenderedDab &dab, state->dabsQueue) {
                const QVector<QPoint> normalizationOrigins =
                    KisWrappedRect::normalizationOriginsForRect(dab.realBounds(), wrapRect, wrapAroundModeAxis);

                Q_FOREACH(const QPoint &pt, normalizationOrigins) {
                    KisRenderedDab newDab = dab;

                    newDab.offset = pt;
                    rects.append(KisWrappedRect::clipToWrapRect(newDab.realBounds(), wrapRect, wrapAroundModeAxis));
                    wrappedDabs.append(newDab);
                }
            }

            state->dabsQueue = wrappedDabs;

        } else {
            // just get all rects
            Q_FOREACH (const KisRenderedDab &dab, state->dabsQueue) {
                rects.append(dab.realBounds());
            }
        }

        // split/merge rects into non-overlapping areas
        rects = KisPaintOpUtils::splitDabsIntoRects(rects,
                                                    idealNumRects, diameter, spacing);

        state->allDirtyRects = rects;

        Q_FOREACH (const KisRenderedDab &dab, state->dabsQueue) {
            state->dabPoints.append(dab.realBounds().center());
        }

        state->dabRenderingTimer.start();

        addDabPaintingJobs(rects, state, jobs);

        /**
         * After the dab has been rendered once, we should mirror it either one
         * (h __or__ v) or three (h __and__ v) times. This sequence of 'if's achieves
         * the goal without any extra copying. Please note that it has __no__ 'else'
         * branches, which is done intentionally!
         */
        if (state->painter->hasHorizontalMirroring()) {
            addMirroringJobs(Qt::Horizontal, rects, state, jobs);
        }

        if (state->painter->hasVerticalMirroring()) {
            addMirroringJobs(Qt::Vertical, rects, state, jobs);
        }

        if (state->painter->hasHorizontalMirroring() && state->painter->hasVerticalMirroring()) {
            addMirroringJobs(Qt::Horizontal, rects, state, jobs);
        }

        KritaUtils::addJobSequential(jobs,
                [state, this, someDabsAreStillInQueue, stoppedByByteLimit, gpuPath] () {
                    Q_FOREACH(const QRect &rc, state->allDirtyRects) {
                        state->painter->addDirtyRect(rc);
                    }
                    KisPaintTrace::link("batch.dirty_recorded",
                                        state->painter,
                                        state->paintTraceBatch,
                                        KisPaintTrace::currentJob());

                    state->painter->setAverageOpacity(state->dabsQueue.last().averageOpacity);
                    state->painter->recordPaintTraceBatch(state->paintTraceBatch);

                    const int updateRenderingTime = state->dabRenderingTimer.elapsed();
                    const qreal dabRenderingTime = m_dabExecutor->averageDabRenderingTime();

                    if (KisGpuBrushPainter::isEnabled() && qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1
                        && updateRenderingTime >= 20) {
                        qInfo() << "GPU brush: slow update ms" << updateRenderingTime << "dabs"
                                << state->dabsQueue.size() << "average dab generation ms" << dabRenderingTime
                                << "combined mirrors" << state->gpuMirrorsPainted << "more prepared dabs"
                                << someDabsAreStillInQueue << "mode" << state->painter->compositeOpId() << "channels"
                                << state->painter->channelFlags();
                    }

                    m_avgNumDabs(state->dabsQueue.size());

                    const qreal currentUpdateTimePerDab = qreal(updateRenderingTime) / state->dabsQueue.size();
                    m_avgUpdateTimePerDab(currentUpdateTimePerDab);

                    /**
                     * NOTE: using currentUpdateTimePerDab in the calculation for the next update time instead
                     *       of the average one makes rendering speed about 40% faster. It happens because the
                     *       adaptation period is shorter than if it used
                     */
                    const qreal totalRenderingTimePerDab = dabRenderingTime + currentUpdateTimePerDab;

                    const int approxDabRenderingTime =
                        qreal(totalRenderingTimePerDab) * m_avgNumDabs.rollingMean() / m_idealNumRects;

                    m_currentUpdatePeriod =
                        someDabsAreStillInQueue ? m_minUpdatePeriod :
                        qBound(m_minUpdatePeriod, int(1.5 * approxDabRenderingTime), m_maxUpdatePeriod);

                    // GPU engine (Solstice): a GPU batch cut only by its source
                    // byte budget (e.g. rotated large dabs) left rendered dabs
                    // behind. Waiting the minimum period for each such batch
                    // caps fast strokes below their dab rate, so take the next
                    // batch as soon as the stroke asks again. Only one batch is
                    // in flight at a time (m_updateSharedState).
                    if (someDabsAreStillInQueue && stoppedByByteLimit) {
                        m_currentUpdatePeriod = 0;
                    }
                    if (gpuPath) {
                        m_currentUpdatePeriod = gpuMinimumUpdatePeriod();
                    }


                    { // debug chunk
//                        ENTER_FUNCTION() << ppVar(state->allDirtyRects.size()) << ppVar(state->dabsQueue.size()) << ppVar(dabRenderingTime) << ppVar(updateRenderingTime);
//                        ENTER_FUNCTION() << ppVar(m_currentUpdatePeriod) << ppVar(someDabsAreStillInQueue);
                    }

                    // release all the dab devices
                    state->dabsQueue.clear();

                    m_updateSharedState.clear();
                }
        );
    } else if (m_updateSharedState && hasPreparedDabsAtStart) {
        someDabsAreStillInQueue = true;
    }

    return std::make_pair(m_currentUpdatePeriod, someDabsAreStillInQueue);
}

KisSpacingInformation KisBrushOp::updateSpacingImpl(const KisPaintInformation &info) const
{
    const qreal scale =
        applyMinimumDabSize(m_sizeOption.apply(info)) * KisLodTransform::lodToScale(painter()->device());
    qreal rotation = m_rotationOption.apply(info);
    return effectiveSpacing(scale, rotation, &m_airbrushData, &m_spacingOption, info);
}

KisTimingInformation KisBrushOp::updateTimingImpl(const KisPaintInformation &info) const
{
    return KisPaintOpPluginUtils::effectiveTiming(&m_airbrushData, &m_rateOption, info);
}

void KisBrushOp::paintLine(const KisPaintInformation& pi1, const KisPaintInformation& pi2, KisDistanceInformation *currentDistance)
{
    if (m_sharpnessOption.isChecked() && m_brush && (m_brush->width() == 1) && (m_brush->height() == 1)) {

        if (!m_lineCacheDevice) {
            m_lineCacheDevice = source()->createCompositionSourceDevice();
        }
        else {
            m_lineCacheDevice->clear();
        }

        KisPainter p(m_lineCacheDevice);
        p.setPaintColor(painter()->paintColor());
        p.drawDDALine(pi1.pos(), pi2.pos());

        QRect rc = m_lineCacheDevice->extent();
        painter()->bitBlt(rc.x(), rc.y(), m_lineCacheDevice, rc.x(), rc.y(), rc.width(), rc.height());
    //fixes Bug 338011
    painter()->renderMirrorMask(rc, m_lineCacheDevice);
    }
    else {
        KisPaintOp::paintLine(pi1, pi2, currentDistance);
    }
}
