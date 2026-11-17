/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "freehand_stroke.h"
#include "KisPaintTrace.h"

#include <QElapsedTimer>
#include <QThread>
#include <QApplication>

#include "kis_canvas_resource_provider.h"
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include "kis_painter.h"
#include "kis_paintop.h"

#include "kis_update_time_monitor.h"

#include <brushengine/kis_stroke_random_source.h>
#include <KisRunnableStrokeJobsInterface.h>
#include <KisRunnableStrokeJobUtils.h>
#include "FreehandStrokeRunnableJobDataWithUpdate.h"
#include <mutex>

#include "KisStrokeEfficiencyMeasurer.h"
#include <KisStrokeSpeedMonitor.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/KisMaskedFreehandStrokePainter.h>

#include "brushengine/kis_paintop_utils.h"
#include "KisAsynchronousStrokeUpdateHelper.h"
#include "KisBrushStrokeLayer.h"
#include "kis_command_utils.h"
#include <QRandomGenerator>
#include <KisUsageLogger.h>

struct FreehandStrokeStrategy::Private
{
    Private(KisResourcesSnapshotSP _resources)
        : resources(_resources),
          needsAsynchronousUpdates(_resources->presetNeedsAsynchronousUpdates())
    {
        if (needsAsynchronousUpdates) {
            timeSinceLastUpdate.start();
        }
    }

    Private(const Private &rhs)
        : randomSource(rhs.randomSource),
          resources(rhs.resources),
          needsAsynchronousUpdates(rhs.needsAsynchronousUpdates)
    {
        if (needsAsynchronousUpdates) {
            timeSinceLastUpdate.start();
        }
        efficiencyMeasurer.setEnabled(rhs.efficiencyMeasurer.isEnabled());
    }

    KisStrokeRandomSource randomSource;
    KisResourcesSnapshotSP resources;

    KisStrokeEfficiencyMeasurer efficiencyMeasurer;

    QElapsedTimer timeSinceLastUpdate;
    // GPU engine (Solstice, phase 4.88): attempt the first update at once;
    // the paint op then returns its own period. Since 2026-10-10 on the CPU
    // path too (the first dabs waited 40 ms).
    int currentUpdatePeriod = -1;

    const bool needsAsynchronousUpdates = false;
    std::mutex updateEntryMutex;

    // Solstice: the stroke recorded for a brush stroke layer
    // (docs/agent/brush-stroke-layer.md); not copied to level of
    // detail clones
    KisBrushStrokeLayerSP recordingLayer;
    QSharedPointer<KisRecordedBrushStroke> recording;
    std::mutex recordingMutex;
    /// the layer whose recorded change this stroke has begun and not yet
    /// handed to its undo command (KisRecordedChangeScopeCommand)
    KisBrushStrokeLayerSP recordedChangeLayer;
};

FreehandStrokeStrategy::FreehandStrokeStrategy(KisResourcesSnapshotSP resources,
                                               KisFreehandStrokeInfo *strokeInfo,
                                               const KUndo2MagicString &name,
                                               Flags flags)
    : KisPainterBasedStrokeStrategy(QLatin1String("FREEHAND_STROKE"), name,
                                    resources, strokeInfo),
      m_d(new Private(resources))
{
    init(flags);
    initRecording({strokeInfo});
}

FreehandStrokeStrategy::FreehandStrokeStrategy(KisResourcesSnapshotSP resources,
                                               QVector<KisFreehandStrokeInfo*> strokeInfos,
                                               const KUndo2MagicString &name,
                                               Flags flags)
    : KisPainterBasedStrokeStrategy(QLatin1String("FREEHAND_STROKE"), name,
                                    resources, strokeInfos),
      m_d(new Private(resources))
{
    init(flags);
    initRecording(strokeInfos);
}

FreehandStrokeStrategy::FreehandStrokeStrategy(const FreehandStrokeStrategy &rhs, int levelOfDetail)
    : KisPainterBasedStrokeStrategy(rhs, levelOfDetail),
      m_d(new Private(*rhs.m_d))
{
    m_d->randomSource.setLevelOfDetail(levelOfDetail);
}

FreehandStrokeStrategy::~FreehandStrokeStrategy()
{
    endRecordedChange();

    KisStrokeSpeedMonitor::instance()->notifyStrokeFinished(m_d->efficiencyMeasurer.averageCursorSpeed(),
                                                            m_d->efficiencyMeasurer.averageRenderingSpeed(),
                                                            m_d->efficiencyMeasurer.averageFps(),
                                                            m_d->resources->currentPaintOpPreset());

    KisUpdateTimeMonitor::instance()->endStrokeMeasure();
}

void FreehandStrokeStrategy::init(Flags flags)
{
    setSupportsWrapAroundMode(true);
    setSupportsMaskingBrush(true);
    setSupportsIndirectPainting(true);
    setSupportsContinuedInterstrokeData(flags & SupportsContinuedInterstrokeData);
    setSupportsTimedMergeId(flags & SupportsTimedMergeId);

    enableJob(KisSimpleStrokeStrategy::JOB_DOSTROKE);

    if (m_d->needsAsynchronousUpdates) {
        /**
         * In case the paintop uses asynchronous updates, we should set priority to it,
         * because FPS is controlled separately, not by the queue's merging algorithm.
         */
        setBalancingRatioOverride(0.01); // set priority to updates
    }

    KisUpdateTimeMonitor::instance()->startStrokeMeasure();
    m_d->efficiencyMeasurer.setEnabled(KisStrokeSpeedMonitor::instance()->haveStrokeSpeedMeasurement());
}

void FreehandStrokeStrategy::setPreviewRandomSeed(int seed)
{
    m_d->randomSource.setSeed(seed);
    if (m_d->recording) {
        m_d->recording->seed = seed;
    }
}

void FreehandStrokeStrategy::initRecording(const QVector<KisFreehandStrokeInfo *> &strokeInfos)
{
    KisResourcesSnapshotSP resources = m_d->resources;
    KisBrushStrokeLayer *layer = dynamic_cast<KisBrushStrokeLayer *>(resources->currentNode().data());
    // an animated layer's record is never drawn again (its frames differ);
    // a through-layer erase is not part of the layer's own strokes
    if (!layer || !layer->paintDevice() || layer->paintDevice()->keyframeChannel()
        || !resources->currentPaintOpPreset() || !resources->throughLayerTargets().isEmpty()) {
        return;
    }

    m_d->recordingLayer = layer;
    m_d->recording.reset(new KisRecordedBrushStroke());
    // dabs rendered on several threads may differ with the threads' timing
    setSequentialDabRendering(true);
    KisRecordedBrushStroke &stroke = *m_d->recording;
    stroke.preset = resources->currentPaintOpPreset()->clone().dynamicCast<KisPaintOpPreset>();
    stroke.fgColor = resources->currentFgColor();
    stroke.bgColor = resources->currentBgColor();
    stroke.compositeOpId = resources->compositeOpId();
    stroke.opacity = resources->opacity();
    stroke.effectiveZoom = resources->effectiveZoom();
    stroke.pattern = resources->currentPattern();
    stroke.gradient = resources->currentGradient();
    stroke.strokeInfoCount = strokeInfos.size();
    for (KisFreehandStrokeInfo *info : strokeInfos) {
        const KisDistanceInformation *distance = info->dragDistance;
        KisRecordedBrushStroke::Start start;
        start.hasLastDab = distance->hasLastDabInformation();
        if (start.hasLastDab) {
            start.lastPosition = distance->lastPosition();
            start.lastAngle = distance->lastDrawingAngle();
        }
        start.spacingUpdateInterval = distance->getSpacingInterval();
        start.timingUpdateInterval = distance->getTimingUpdateInterval();
        start.dabSeqNo = distance->currentDabSeqNo();
        stroke.starts << start;
    }
    stroke.mirrorHorizontal = resources->mirrorMaskHorizontal();
    stroke.mirrorVertical = resources->mirrorMaskVertical();
    stroke.mirrorAxesCenter = resources->axesCenter();
    // the device offset is read when the stroke starts (initStrokeCallback()):
    // a stroke queued before this one may still move the layer

    // a known seed, so that the stroke can be drawn again the same way
    stroke.seed = int(QRandomGenerator::global()->bounded(1, std::numeric_limits<int>::max()));
    m_d->randomSource.setSeed(stroke.seed);
}

void FreehandStrokeStrategy::recordJob(const Data &data)
{
    KisRecordedBrushStroke::Job job;
    job.type = data.type;
    job.strokeInfoId = data.strokeInfoId;
    job.pi1 = data.pi1;
    job.pi2 = data.pi2;
    job.control1 = data.control1;
    job.control2 = data.control2;

    std::lock_guard<std::mutex> lock(m_d->recordingMutex);
    if (data.type == Data::POINT || data.type == Data::LINE || data.type == Data::CURVE) {
        m_d->recording->jobs.append(job);
    } else {
        // a shape the stroke cannot draw again: not recorded at all
        KisUsageLogger::log(
            QStringLiteral("Brush stroke layer: a stroke with a job of type %1 is not recorded").arg(data.type));
        // its pixels are not the record: the layer checks them
        m_d->recordingLayer->markChangeUnrecorded();
        m_d->recording.reset();
        m_d->recordingLayer.clear();
    }
}

void FreehandStrokeStrategy::endRecordedChange()
{
    if (m_d->recordedChangeLayer) {
        m_d->recordedChangeLayer->endRecordedChange();
        m_d->recordedChangeLayer.clear();
    }
}

void FreehandStrokeStrategy::initStrokeCallback()
{
    if (m_d->recording) {
        KisPaintDeviceSP device = m_d->recordingLayer->paintDevice();
        m_d->recording->deviceOffset = QPoint(device->x(), device->y());
        // the stroke's pixel changes belong to its record
        m_d->recordingLayer->beginRecordedChange();
        m_d->recordedChangeLayer = m_d->recordingLayer;
    }
    KisPainterBasedStrokeStrategy::initStrokeCallback();
    m_d->efficiencyMeasurer.notifyRenderingStarted();
}

void FreehandStrokeStrategy::finishStrokeCallback()
{
    m_d->efficiencyMeasurer.notifyRenderingFinished();

    if (m_d->recording && !m_d->recording->jobs.isEmpty()) {
        // the undo command closes the stroke's recorded change after the
        // last of its pixels is merged, and brackets undo and redo
        KisBrushStrokeLayer *layer = m_d->recordingLayer.data();
        const bool scopeOpen = m_d->recordedChangeLayer == m_d->recordingLayer;
        KisCommandUtils::CompositeCommand *command = new KisCommandUtils::CompositeCommand();
        command->addCommand(
            new KisRecordedChangeScopeCommand(layer, KisRecordedChangeScopeCommand::Opening, scopeOpen));
        command->addCommand(new KisRecordBrushStrokeCommand(layer, m_d->recording));
        setAdditionalUndoCommand(
            command,
            new KisRecordedChangeScopeCommand(layer, KisRecordedChangeScopeCommand::Closing, scopeOpen));
        if (scopeOpen) {
            m_d->recordedChangeLayer.clear();
        }
    }
    m_d->recording.reset();
    m_d->recordingLayer.clear();
    // not recorded: the rest of the stroke's changes are checked
    endRecordedChange();

    for (KisNodeSP node : m_d->resources->throughLayerTargets()) {
        if (KisBrushStrokeLayer *layer = dynamic_cast<KisBrushStrokeLayer *>(node.data())) {
            layer->markChangeUnrecorded();
        }
    }

    KisPainterBasedStrokeStrategy::finishStrokeCallback();
}

void FreehandStrokeStrategy::cancelStrokeCallback()
{
    m_d->recording.reset();
    m_d->recordingLayer.clear();
    KisPainterBasedStrokeStrategy::cancelStrokeCallback();
    endRecordedChange();
}


void FreehandStrokeStrategy::doStrokeCallback(KisStrokeJobData *data)
{
    KisPaintTrace::Scope trace("stroke.run", this, data);
    if (KisAsynchronousStrokeUpdateHelper::UpdateData *d =
            dynamic_cast<KisAsynchronousStrokeUpdateHelper::UpdateData*>(data)) {

        // this job is lod-clonable in contrast to FreehandStrokeRunnableJobDataWithUpdate!
        tryDoUpdate(d->forceUpdate);

    } else if (Data *d = dynamic_cast<Data*>(data)) {
        KisMaskedFreehandStrokePainter *maskedPainter = this->maskedPainter(d->strokeInfoId);

        if (m_d->recording) {
            recordJob(*d);
        }

        KisUpdateTimeMonitor::instance()->reportPaintOpPreset(maskedPainter->preset());
        KisRandomSourceSP rnd = m_d->randomSource.source();
        KisPerStrokeRandomSourceSP strokeRnd = m_d->randomSource.perStrokeSource();

        switch(d->type) {
        case Data::POINT:
            d->pi1.setRandomSource(rnd);
            d->pi1.setPerStrokeRandomSource(strokeRnd);
            maskedPainter->paintAt(d->pi1);
            m_d->efficiencyMeasurer.addSample(d->pi1.pos());
            break;
        case Data::LINE:
            d->pi1.setRandomSource(rnd);
            d->pi2.setRandomSource(rnd);
            d->pi1.setPerStrokeRandomSource(strokeRnd);
            d->pi2.setPerStrokeRandomSource(strokeRnd);
            maskedPainter->paintLine(d->pi1, d->pi2);
            m_d->efficiencyMeasurer.addSample(d->pi2.pos());
            break;
        case Data::CURVE:
            d->pi1.setRandomSource(rnd);
            d->pi2.setRandomSource(rnd);
            d->pi1.setPerStrokeRandomSource(strokeRnd);
            d->pi2.setPerStrokeRandomSource(strokeRnd);
            maskedPainter->paintBezierCurve(d->pi1,
                                         d->control1,
                                         d->control2,
                                         d->pi2);
            m_d->efficiencyMeasurer.addSample(d->pi2.pos());
            break;
        case Data::POLYLINE:
            maskedPainter->paintPolyline(d->points, 0, d->points.size());
            m_d->efficiencyMeasurer.addSamples(d->points);
            break;
        case Data::POLYGON:
            maskedPainter->paintPolygon(d->points);
            m_d->efficiencyMeasurer.addSamples(d->points);
            break;
        case Data::RECT:
            maskedPainter->paintRect(d->rect);
            m_d->efficiencyMeasurer.addSample(d->rect.topLeft());
            m_d->efficiencyMeasurer.addSample(d->rect.topRight());
            m_d->efficiencyMeasurer.addSample(d->rect.bottomRight());
            m_d->efficiencyMeasurer.addSample(d->rect.bottomLeft());
            break;
        case Data::ELLIPSE:
            maskedPainter->paintEllipse(d->rect);
            // TODO: add speed measures
            break;
        case Data::PAINTER_PATH:
            maskedPainter->paintPainterPath(d->path);
            // TODO: add speed measures
            break;
        case Data::QPAINTER_PATH:
            maskedPainter->drawPainterPath(d->path, d->pen);
            break;
        case Data::QPAINTER_PATH_FILL:
            maskedPainter->drawAndFillPainterPath(d->path, d->pen, d->customColor);
            break;
        };

        tryDoUpdate();
    } else {
        KisPainterBasedStrokeStrategy::doStrokeCallback(data);

        FreehandStrokeRunnableJobDataWithUpdate *dataWithUpdate =
            dynamic_cast<FreehandStrokeRunnableJobDataWithUpdate*>(data);

        if (dataWithUpdate) {
            tryDoUpdate();
        }
    }
}

void FreehandStrokeStrategy::tryDoUpdate(bool forceEnd)
{
    // we should enter this function only once!
    std::unique_lock<std::mutex> entryLock(m_d->updateEntryMutex, std::try_to_lock);
    if (!entryLock.owns_lock()) return;

    if (m_d->needsAsynchronousUpdates) {
        if (forceEnd || m_d->timeSinceLastUpdate.elapsed() > m_d->currentUpdatePeriod) {
            m_d->timeSinceLastUpdate.restart();

            for (int i = 0; i < numMaskedPainters(); i++) {
                KisMaskedFreehandStrokePainter *maskedPainter = this->maskedPainter(i);

                // TODO: well, we should count all N simultaneous painters for FPS rate!
                QVector<KisRunnableStrokeJobData*> jobs;

                bool needsMoreUpdates = false;

                std::tie(m_d->currentUpdatePeriod, needsMoreUpdates) =
                    maskedPainter->doAsynchronousUpdate(jobs);
                const bool startedBatch = !jobs.isEmpty();

                if (!jobs.isEmpty() ||
                    maskedPainter->hasDirtyRegion() ||
                    (forceEnd && needsMoreUpdates)) {

                    KritaUtils::addJobSequential(jobs,
                        [this] () {
                            this->issueSetDirtySignals();
                        }
                    );

                    if (forceEnd && needsMoreUpdates) {
                        KritaUtils::addJobSequential(jobs,
                            [this] () {
                                this->tryDoUpdate(true);
                            }
                        );
                    } else if (startedBatch) {
                        // GPU engine (Solstice, phase 4.89): dabs finished while
                        // this batch is in flight get no trigger until the next
                        // input. Check again once it ends; this is a no-op when
                        // nothing is ready or the paint op's period has not passed.
                        // Since 2026-10-10 on the CPU path too.
                        KritaUtils::addJobSequential(jobs, [this]() {
                            this->tryDoUpdate();
                        });
                    }


                    runnableJobsInterface()->addRunnableJobs(jobs);
                    m_d->efficiencyMeasurer.notifyFrameRenderingStarted();
                }

            }
        }
    } else {
        issueSetDirtySignals();
    }


}

void FreehandStrokeStrategy::issueSetDirtySignals()
{
    const quint64 dirtyId = KisPaintTrace::nextId();
    KisPaintTrace::FlowScope dirtyFlow(dirtyId);
    KisPaintTrace::link("dirty.dispatch", this, dirtyId, KisPaintTrace::currentJob());
    KisPaintTrace::Scope trace("stroke.dirty", this);
    QVector<QRect> dirtyRects;

    for (int i = 0; i < numMaskedPainters(); i++) {
        KisMaskedFreehandStrokePainter *maskedPainter = this->maskedPainter(i);
        dirtyRects.append(maskedPainter->takeDirtyRegion());
    }

    const auto traceDirtyRects = [this, dirtyId](const char *name, const QVector<QRect> &rects) {
        if (!KisPaintTrace::enabled())
            return;
        const auto node = targetNode();
        const int lod = node->projection()->defaultBounds()->currentLevelOfDetail();
        if (rects.isEmpty()) {
            KisPaintTrace::rectangle(name, node.data(), dirtyId, QRect(), lod);
        }
        for (const QRect &rect : rects) {
            KisPaintTrace::rectangle(name, node.data(), dirtyId, rect, lod);
        }
    };
    traceDirtyRects("dirty.source_rect", dirtyRects);

    if (needsMaskingUpdates()) {

        // optimize the rects so that they would never intersect with each other!
        // that is a mandatory step for the multithreaded execution of merging jobs

        // sanity check: updates from the brush should have already been normalized
        //               to the wrapping rect
        const KisDefaultBoundsBaseSP defaultBounds = targetNode()->projection()->defaultBounds();
        if (defaultBounds->wrapAroundMode()) {
            const QRect wrapRect = defaultBounds->imageBorderRect();
            for (auto it = dirtyRects.begin(); it != dirtyRects.end(); ++it) {
                KIS_SAFE_ASSERT_RECOVER(wrapRect.contains(*it)) {
                    ENTER_FUNCTION() << ppVar(*it) << ppVar(wrapRect);
                    *it = *it & wrapRect;
                }
            }
        }

        const int maxPatchSizeForMaskingUpdates = 64;
        const QRect totalRect =
            std::accumulate(dirtyRects.constBegin(), dirtyRects.constEnd(), QRect(), std::bit_or<QRect>());

        dirtyRects = KisPaintOpUtils::splitAndFilterDabRect(totalRect, dirtyRects, maxPatchSizeForMaskingUpdates);

        QVector<KisRunnableStrokeJobData*> jobs = doMaskingBrushUpdates(dirtyRects);

        KritaUtils::addJobSequential(jobs, [this, dirtyRects, dirtyId, traceDirtyRects]() {
            KisPaintTrace::FlowScope dirtyFlow(dirtyId);
            traceDirtyRects("dirty.submitted_rect", dirtyRects);
            this->updateStrokeNodes(dirtyRects);
        });

        runnableJobsInterface()->addRunnableJobs(jobs);

    } else {
        traceDirtyRects("dirty.submitted_rect", dirtyRects);
        updateStrokeNodes(dirtyRects);
    }

    //KisUpdateTimeMonitor::instance()->reportJobFinished(data, dirtyRects);
}

KisStrokeStrategy* FreehandStrokeStrategy::createLodClone(int levelOfDetail)
{
    if (!m_d->resources->presetAllowsLod()) return 0;
    if (!m_d->resources->currentNode()->supportsLodPainting()) return 0;

    FreehandStrokeStrategy *clone = new FreehandStrokeStrategy(*this, levelOfDetail);
    return clone;
}

void FreehandStrokeStrategy::notifyUserStartedStroke()
{
    m_d->efficiencyMeasurer.notifyCursorMoveStarted();
}

void FreehandStrokeStrategy::notifyUserEndedStroke()
{
    m_d->efficiencyMeasurer.notifyCursorMoveFinished();
}
