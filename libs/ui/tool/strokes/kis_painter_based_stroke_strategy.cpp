/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_painter_based_stroke_strategy.h"

#include <KoColorSpace.h>
#include <KoColor.h>
#include <KoCompositeOp.h>
#include "kis_painter.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_transaction.h"
#include "kis_image.h"
#include "kis_raster_keyframe_channel.h"
#include <kis_distance_information.h>
#include "kis_undo_stores.h"
#include "KisFreehandStrokeInfo.h"
#include "KisMaskedFreehandStrokePainter.h"
#include "KisMaskingBrushRenderer.h"
#include "KisRunnableStrokeJobData.h"
#include "KisAnimAutoKey.h"

#include "gpu/KisGpuMaskingWorker.h"
#include "kis_default_bounds_base.h"
#include "kis_paintop_registry.h"
#include "kis_paintop_preset.h"
#include "kis_paintop_settings.h"

#include <commands_new/kis_saved_commands.h>
#include "kis_command_utils.h"

#include "KisInterstrokeDataFactory.h"
#include "KisInterstrokeDataTransactionWrapperFactory.h"
#include "KisRunnableStrokeJobsInterface.h"
#include "KisRunnableStrokeJobUtils.h"
#include "FreehandStrokeRunnableJobDataWithUpdate.h"
#include <KisStrokeCompatibilityInfo.h>
#include "KisAnimAutoKey.h"

namespace
{
/**
 * Solstice: passes the paint ops' jobs on with the concurrent ones made
 * sequential (setSequentialDabRendering())
 */
class SequentialRunnableJobs : public KisRunnableStrokeJobsInterface
{
public:
    SequentialRunnableJobs(KisRunnableStrokeJobsInterface *target)
        : m_target(target)
    {
    }

    void addRunnableJobs(const QVector<KisRunnableStrokeJobDataBase *> &list) override
    {
        QVector<KisRunnableStrokeJobDataBase *> jobs;
        for (KisRunnableStrokeJobDataBase *job : list) {
            if (job->sequentiality() != KisStrokeJobData::CONCURRENT) {
                jobs << job;
                continue;
            }
            // owned by the wrapper, so that a job dropped by cancelling
            // the stroke is deleted too
            QSharedPointer<KisRunnableStrokeJobDataBase> wrapped(job);
            auto run = [wrapped]() {
                wrapped->run();
            };
            // a dab job that asks for a canvas update when it is done
            // (FreehandStrokeStrategy) still does
            KisRunnableStrokeJobData *sequential = dynamic_cast<FreehandStrokeRunnableJobDataWithUpdate *>(job)
                ? new FreehandStrokeRunnableJobDataWithUpdate(run, KisStrokeJobData::SEQUENTIAL, job->exclusivity())
                : new KisRunnableStrokeJobData(run, KisStrokeJobData::SEQUENTIAL, job->exclusivity());
            sequential->setLevelOfDetailOverride(job->levelOfDetailOverride());
            jobs << sequential;
        }
        m_target->addRunnableJobs(jobs);
    }

private:
    KisRunnableStrokeJobsInterface *m_target;
};
} // namespace

KisPainterBasedStrokeStrategy::KisPainterBasedStrokeStrategy(const QLatin1String &id,
                                                             const KUndo2MagicString &name,
                                                             KisResourcesSnapshotSP resources,
                                                             QVector<KisFreehandStrokeInfo*> strokeInfos)
    : KisRunnableBasedStrokeStrategy(id, name),
      m_resources(resources),
      m_strokeInfos(strokeInfos),
      m_transaction(0),
      m_useMergeID(false),
      m_supportsMaskingBrush(false),
      m_supportsIndirectPainting(false)
{
    init();
}

KisPainterBasedStrokeStrategy::KisPainterBasedStrokeStrategy(const QLatin1String &id,
                                                             const KUndo2MagicString &name,
                                                             KisResourcesSnapshotSP resources,
                                                             KisFreehandStrokeInfo *strokeInfo)
    : KisRunnableBasedStrokeStrategy(id, name),
      m_resources(resources),
      m_strokeInfos(QVector<KisFreehandStrokeInfo*>() <<  strokeInfo),
      m_transaction(0),
      m_useMergeID(false),
      m_supportsMaskingBrush(false),
      m_supportsIndirectPainting(false)
{
    init();
}

KisPainterBasedStrokeStrategy::~KisPainterBasedStrokeStrategy()
{
}

void KisPainterBasedStrokeStrategy::init()
{
    enableJob(KisSimpleStrokeStrategy::JOB_INIT);
    enableJob(KisSimpleStrokeStrategy::JOB_FINISH);
    enableJob(KisSimpleStrokeStrategy::JOB_CANCEL, true, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);

    enableJob(KisSimpleStrokeStrategy::JOB_SUSPEND);
    enableJob(KisSimpleStrokeStrategy::JOB_RESUME);
}

KisPainterBasedStrokeStrategy::KisPainterBasedStrokeStrategy(const KisPainterBasedStrokeStrategy &rhs, int levelOfDetail)
    : KisRunnableBasedStrokeStrategy(rhs),
      m_resources(rhs.m_resources),
      m_useMergeID(rhs.m_useMergeID),
      m_supportsMaskingBrush(rhs.m_supportsMaskingBrush),
      m_supportsIndirectPainting(rhs.m_supportsIndirectPainting),
      m_supportsContinuedInterstrokeData(rhs.m_supportsContinuedInterstrokeData)
{
    Q_FOREACH (KisFreehandStrokeInfo *info, rhs.m_strokeInfos) {
        m_strokeInfos.append(new KisFreehandStrokeInfo(info, levelOfDetail));
    }

    KIS_ASSERT_RECOVER_NOOP(
        rhs.m_maskStrokeInfos.isEmpty() &&
        !rhs.m_transaction &&
        !rhs.m_targetDevice &&
        !rhs.m_activeSelection &&
        "After the stroke has been started, no copying must happen");
}

KisPaintDeviceSP KisPainterBasedStrokeStrategy::targetDevice() const
{
    return m_targetDevice;
}

KisSelectionSP KisPainterBasedStrokeStrategy::activeSelection() const
{
    return m_activeSelection;
}

KisMaskedFreehandStrokePainter *KisPainterBasedStrokeStrategy::maskedPainter(int strokeInfoId)
{
    return m_maskedPainters[strokeInfoId];
}

int KisPainterBasedStrokeStrategy::numMaskedPainters() const
{
    return m_maskedPainters.size();
}

bool KisPainterBasedStrokeStrategy::needsMaskingUpdates() const
{
    return bool(m_maskingBrushRenderer);
}

QVector<KisRunnableStrokeJobData *> KisPainterBasedStrokeStrategy::doMaskingBrushUpdates(const QVector<QRect> &rects)
{
    QVector<KisRunnableStrokeJobData *> jobs;
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_maskingBrushRenderer, jobs);

    // Solstice (GPU engine): the stroke is painted with the GPU brush and the
    // composite runs on the GPU, for all the rects at once; if the GPU cannot
    // run it, the CPU composites (reading the stroke back first)
    if (m_gpuMasking && !rects.isEmpty()) {
        KritaUtils::addJobSequential(jobs, [this, rects]() {
            QRect all;
            for (const QRect &rc : rects) {
                all |= rc;
            }
            KisMaskingBrushRenderer *renderer = this->m_maskingBrushRenderer.data();
            if (!KisGpuMaskingWorker::apply(renderer->strokeDevice(),
                                            renderer->maskDevice(),
                                            renderer->dstDevice(),
                                            all,
                                            renderer->compositeOpId())) {
                for (const QRect &rc : rects) {
                    renderer->updateProjection(rc);
                }
            }
        });
        return jobs;
    }

    Q_FOREACH (const QRect &rc, rects) {
        KritaUtils::addJobConcurrent(jobs,
            [this, rc] () {
                this->m_maskingBrushRenderer->updateProjection(rc);
            }
        );
    }

    return jobs;
}

void KisPainterBasedStrokeStrategy::setSupportsMaskingBrush(bool value)
{
    m_supportsMaskingBrush = value;
}

bool KisPainterBasedStrokeStrategy::supportsMaskingBrush() const
{
    return m_supportsMaskingBrush;
}

void KisPainterBasedStrokeStrategy::setSupportsIndirectPainting(bool value)
{
    m_supportsIndirectPainting = value;
}

bool KisPainterBasedStrokeStrategy::supportsIndirectPainting() const
{
    return m_supportsIndirectPainting;
}

bool KisPainterBasedStrokeStrategy::supportsContinuedInterstrokeData() const
{
    return m_supportsContinuedInterstrokeData;
}

void KisPainterBasedStrokeStrategy::setSupportsContinuedInterstrokeData(bool value)
{
    m_supportsContinuedInterstrokeData = value;
}

bool KisPainterBasedStrokeStrategy::supportsTimedMergeId() const
{
    return m_useMergeID;
}

void KisPainterBasedStrokeStrategy::setSupportsTimedMergeId(bool value)
{
    m_useMergeID = value;
}

void KisPainterBasedStrokeStrategy::initPainters(KisPaintDeviceSP targetDevice,
                                                 KisPaintDeviceSP maskingDevice,
                                                 KisSelectionSP selection,
                                                 bool hasIndirectPainting,
                                                 const QString &indirectPaintingCompositeOp)
{
    Q_FOREACH (KisFreehandStrokeInfo *info, m_strokeInfos) {
        KisPainter *painter = info->painter;

        painter->begin(targetDevice, !hasIndirectPainting ? selection : nullptr);
        if (m_sequentialDabRendering && !m_sequentialJobs) {
            m_sequentialJobs.reset(new SequentialRunnableJobs(runnableJobsInterface()));
        }
        painter->setRunnableStrokeJobsInterface(m_sequentialDabRendering ? m_sequentialJobs.data()
                                                                         : runnableJobsInterface());
        m_resources->setupPainter(painter);

        if(hasIndirectPainting) {
            painter->setCompositeOpId(indirectPaintingCompositeOp);
            if (!m_throughLayerBuildUp) {
                painter->setOpacityToUnit();
            }
            painter->setChannelFlags(QBitArray());
        }
    }

    if (maskingDevice) {
        for (int i = 0; i < m_strokeInfos.size(); i++) {
            KisFreehandStrokeInfo *maskingInfo =
                new KisFreehandStrokeInfo(*m_strokeInfos[i]->dragDistance);

            KisPainter *painter = maskingInfo->painter;

            painter->begin(maskingDevice, nullptr);
            m_resources->setupMaskingBrushPainter(painter);

            KIS_SAFE_ASSERT_RECOVER_NOOP(hasIndirectPainting);
            m_maskStrokeInfos.append(maskingInfo);
        }
    }

    for (int i = 0; i < m_strokeInfos.size(); i++) {
        m_maskedPainters.append(
            new KisMaskedFreehandStrokePainter(m_strokeInfos[i],
                                               !m_maskStrokeInfos.isEmpty() ?
                                                   m_maskStrokeInfos[i] : nullptr));
    }
}

void KisPainterBasedStrokeStrategy::deletePainters()
{
    Q_FOREACH (KisFreehandStrokeInfo *info, m_strokeInfos) {
        delete info;
    }

    m_strokeInfos.clear();

    Q_FOREACH (KisFreehandStrokeInfo *info, m_maskStrokeInfos) {
        delete info;
    }

    m_maskStrokeInfos.clear();

    Q_FOREACH (KisMaskedFreehandStrokePainter *info, m_maskedPainters) {
        delete info;
    }

    m_maskedPainters.clear();
}

KisNodeList KisPainterBasedStrokeStrategy::strokeNodes() const
{
    const KisNodeList targets = m_resources->throughLayerTargets();
    return targets.isEmpty() ? KisNodeList{m_resources->currentNode()} : targets;
}

void KisPainterBasedStrokeStrategy::attachThroughLayerTarget(KisNodeSP node, KisPaintDeviceSP device)
{
    KisPaintLayer *layer = dynamic_cast<KisPaintLayer *>(node.data());
    KIS_SAFE_ASSERT_RECOVER_RETURN(layer);

    layer->setTemporaryTarget(device);
    layer->setTemporaryCompositeOp(m_resources->compositeOpId());
    layer->setTemporaryOpacity(m_throughLayerBuildUp ? OPACITY_OPAQUE_F : m_resources->opacity());
    layer->setTemporarySelection(layer->selection());

    // an alpha locked layer is a target only when the tool is told to erase
    // it, and erasing changes nothing but alpha. The Erase blend mode ignores
    // the alpha lock today on the CPU and the GPU; unlock alpha explicitly so
    // that the erase does not depend on that.
    QBitArray channelFlags = layer->channelLockFlags();
    if (layer->alphaLocked()) {
        channelFlags |= layer->colorSpace()->channelFlags(false, true);
    }
    layer->setTemporaryChannelFlags(channelFlags);
}

void KisPainterBasedStrokeStrategy::updateStrokeNodes(const QVector<QRect> &rects)
{
    if (m_resources->throughLayerTargets().size() <= 1) {
        targetNode()->setDirty(rects);
        return;
    }

    // one walker over the stack; a merge walker per target would composite
    // the whole stack once for each of them
    KisImageSP image = m_resources->image();
    image->refreshGraphAsync(image->root(), rects, image->bounds());
}

void KisPainterBasedStrokeStrategy::initStrokeCallback()
{
    QVector<KisRunnableStrokeJobData*> jobs;

    KritaUtils::addJobSequential(jobs, [this] () {
        QVector<KUndo2Command *> commands;
        for (KisNodeSP node : strokeNodes()) {
            KUndo2Command *autoKeyframeCommand =
                KisAutoKey::tryAutoCreateDuplicatedFrame(node->paintDevice(),
                                                         KisAutoKey::AllowBlankMode |
                                                             KisAutoKey::SupportsLod);
            if (autoKeyframeCommand) {
                commands << autoKeyframeCommand;
            }
        }

        if (commands.size() == 1) {
            m_autokeyCommand.reset(commands.first());
        } else if (!commands.isEmpty()) {
            auto *composite = new KisCommandUtils::CompositeCommand();
            for (KUndo2Command *command : commands) {
                composite->addCommand(command);
            }
            m_autokeyCommand.reset(composite);
        }
    });

    KritaUtils::addJobSequential(jobs, [this] () mutable {
        KisNodeSP node = m_resources->currentNode();
        KisPaintDeviceSP paintDevice = node->paintDevice();
        KisPaintDeviceSP targetDevice = paintDevice;
        KisSelectionSP selection =  m_resources->activeSelection();
        // Solstice: the dabs of a through-layer erase are painted once, on a
        // temporary target that all its layers share
        const bool throughLayers = !m_resources->throughLayerTargets().isEmpty();
        m_throughLayerBuildUp = throughLayers && !m_resources->needsIndirectPainting();
        bool hasIndirectPainting =
            supportsIndirectPainting() && (throughLayers || m_resources->needsIndirectPainting());
        const QString indirectCompositeOp =
            m_throughLayerBuildUp ? QString(COMPOSITE_OVER) : m_resources->indirectPaintingCompositeOp();
        // KisOpacityOption moves the opacity sensors to the flow when it sees
        // a temporary target, which Build-up dabs must not do: they keep
        // their opacity only when the paint ops are created before it is set
        bool attachWhenPaintOpsExist = false;

        if (hasIndirectPainting) {
            KisIndirectPaintingSupport *indirect =
                dynamic_cast<KisIndirectPaintingSupport*>(node.data());

            if (indirect) {
                targetDevice = paintDevice->createCompositionSourceDevice();
                targetDevice->setParentNode(node);
                indirect->setCurrentColor(m_resources->currentFgColor());
                if (throughLayers) {
                    attachWhenPaintOpsExist = m_throughLayerBuildUp;
                    if (!attachWhenPaintOpsExist) {
                        for (KisNodeSP target : strokeNodes()) {
                            attachThroughLayerTarget(target, targetDevice);
                        }
                    }
                } else {
                    indirect->setTemporaryTarget(targetDevice);

                    indirect->setTemporaryCompositeOp(m_resources->compositeOpId());
                    indirect->setTemporaryOpacity(m_resources->opacity());
                    indirect->setTemporarySelection(selection);

                    QBitArray channelLockFlags = m_resources->channelLockFlags();
                    indirect->setTemporaryChannelFlags(channelLockFlags);
                }
            }
            else {
                hasIndirectPainting = false;
            }
        }

        std::unique_ptr<KisInterstrokeDataFactory> interstrokeDataFactory(
            KisPaintOpRegistry::instance()->createInterstrokeDataFactory(m_resources->currentPaintOpPreset()));

        // Solstice: a smudge preset's interstroke data belongs to one layer
        if (throughLayers) {
            interstrokeDataFactory.reset();
        }

        KIS_SAFE_ASSERT_RECOVER(!interstrokeDataFactory || !hasIndirectPainting) {
            interstrokeDataFactory.reset();
        }

        std::unique_ptr<KisInterstrokeDataTransactionWrapperFactory> wrapper;

        if (interstrokeDataFactory) {
            wrapper.reset(new KisInterstrokeDataTransactionWrapperFactory(
                              interstrokeDataFactory.release(),
                              supportsContinuedInterstrokeData()));
        }

        m_transaction.reset(new KisTransaction(KUndo2MagicString(), targetDevice, nullptr,
                                               -1,
                                               wrapper.release()));

        // WARNING: masked brush cannot work without indirect painting mode!
        KIS_SAFE_ASSERT_RECOVER_NOOP(!(supportsMaskingBrush() &&
                                       m_resources->needsMaskingBrushRendering()) || hasIndirectPainting);

        if (hasIndirectPainting &&
            supportsMaskingBrush() &&
            m_resources->needsMaskingBrushRendering()) {

            const QString compositeOpId =
                m_resources->currentPaintOpPreset()->settings()->maskingBrushCompositeOp();

            m_maskingBrushRenderer.reset(new KisMaskingBrushRenderer(targetDevice, compositeOpId));
            // Solstice (GPU engine): with the GPU brush the composite runs on
            // the GPU (KisBrushOp decides the same way); otherwise it writes
            // the temporary target on the CPU, and its Wash preview stays
            // there too
            // (only the Pixel Brush paints with the GPU brush)
            m_gpuMasking = targetDevice->defaultBounds()->currentLevelOfDetail() == 0
                && m_resources->currentPaintOpPreset()->paintOp().id() == QLatin1String("paintbrush")
                && KisGpuMaskingWorker::supports(targetDevice->colorSpace(), compositeOpId);
            for (KisNodeSP target : strokeNodes()) {
                if (auto *indirect = dynamic_cast<KisIndirectPaintingSupport *>(target.data())) {
                    indirect->setTemporaryTargetPaintedOnCpu(!m_gpuMasking);
                }
            }

            initPainters(m_maskingBrushRenderer->strokeDevice(),
                         m_maskingBrushRenderer->maskDevice(),
                         selection,
                         hasIndirectPainting,
                         indirectCompositeOp);

        } else {
            initPainters(targetDevice, nullptr, selection, hasIndirectPainting, indirectCompositeOp);
        }

        if (attachWhenPaintOpsExist) {
            for (KisNodeSP target : strokeNodes()) {
                attachThroughLayerTarget(target, targetDevice);
            }
        }

        m_targetDevice = targetDevice;
        m_activeSelection = selection;

        // sanity check: selection should be applied only once
        if (selection && !m_strokeInfos.isEmpty()) {
            KisIndirectPaintingSupport *indirect =
                dynamic_cast<KisIndirectPaintingSupport*>(node.data());
            KIS_ASSERT_RECOVER_RETURN(hasIndirectPainting || m_strokeInfos.first()->painter->selection());
            // when hasIndirectPainting is true, indirect cannot be null
            KIS_ASSERT_RECOVER_RETURN(!hasIndirectPainting || indirect);
            KIS_ASSERT_RECOVER_RETURN(!hasIndirectPainting || !indirect->temporarySelection() || !m_strokeInfos.first()->painter->selection());
        }
    });

    runnableJobsInterface()->addRunnableJobs(jobs);
}

namespace {

struct MergeableStrokeUndoCommand : KUndo2Command
{
    MergeableStrokeUndoCommand(KisResourcesSnapshotSP resourcesSnapshot)
        : m_compatibilityInfo(*resourcesSnapshot)
    {
    }

    bool timedMergeWith(KUndo2Command *_other) override {
        if(_other->timedId() == this->timedId() && _other->timedId() != -1 ) {
            const bool isCompatible =
                KisSavedCommand::unwrap(_other, [this] (KUndo2Command *cmd) {
                    MergeableStrokeUndoCommand *other =
                        dynamic_cast<MergeableStrokeUndoCommand*>(cmd);
                    return other && m_compatibilityInfo == other->m_compatibilityInfo;
                });

            if (isCompatible) {
                /// NOTE: we are merging the original (potentially) "saved"
                ///       command, to the one we unwrapped above, because
                ///       the unwrapped command is **owned** by the "saved"
                ///       command
                return KUndo2Command::timedMergeWith(_other);
            }
        }
        return false;
    }

    KisStrokeCompatibilityInfo m_compatibilityInfo;
};

} // namespace

void KisPainterBasedStrokeStrategy::finishStrokeCallback()
{
    KisNodeSP node = m_resources->currentNode();
    KisIndirectPaintingSupport *indirect =
        dynamic_cast<KisIndirectPaintingSupport*>(node.data());

    KisPostExecutionUndoAdapter *undoAdapter =
        m_resources->postExecutionUndoAdapter();


    if (!undoAdapter) {
        m_fakeUndoData.reset(new FakeUndoData());
        undoAdapter = m_fakeUndoData->undoAdapter.data();
    }

    QSharedPointer<KUndo2Command> parentCommand;

    if (!m_useMergeID) {
        parentCommand.reset(new KUndo2Command());
    } else {
        parentCommand.reset(new MergeableStrokeUndoCommand(m_resources));
        parentCommand->setTimedID(timedID(this->id()));
    }

    parentCommand->setText(name());
    parentCommand->setTime(m_transaction->undoCommand()->time());
    parentCommand->setEndTime(QTime::currentTime());

    if (m_autokeyCommand) {
        KisCommandUtils::CompositeCommand *wrapper = new KisCommandUtils::CompositeCommand(parentCommand.data());
        wrapper->addCommand(m_autokeyCommand.release());
    }

    if (m_additionalUndoCommand) {
        KisCommandUtils::CompositeCommand *wrapper = new KisCommandUtils::CompositeCommand(parentCommand.data());
        wrapper->addCommand(m_additionalUndoCommand.release());
    }

    if (indirect && indirect->hasTemporaryTarget()) {
        KUndo2MagicString transactionText = m_transaction->text();
        m_transaction->end();
        m_transaction.reset();
        deletePainters();

        QVector<KisRunnableStrokeJobData*> jobs;

        for (KisNodeSP target : strokeNodes()) {
            dynamic_cast<KisIndirectPaintingSupport *>(target.data())
                ->mergeToLayerThreaded(target, parentCommand.data(), KUndo2MagicString(), -1, &jobs);
        }

        // Solstice: after the merge's commands (deleted with the job if
        // it never runs)
        auto closingCommand = std::make_shared<std::unique_ptr<KUndo2Command>>(std::move(m_closingUndoCommand));

        KritaUtils::addJobBarrier(jobs,
            [parentCommand, undoAdapter, closingCommand] () {
                if (*closingCommand) {
                    KisCommandUtils::CompositeCommand *wrapper =
                        new KisCommandUtils::CompositeCommand(parentCommand.data());
                    wrapper->addCommand(closingCommand->release());
                }
                parentCommand->redo();

                if (undoAdapter) {
                    undoAdapter->addCommand(parentCommand);
                }
            });

        /// When the transaction is reset to zero, cancel job does nothing.
        /// Therefore, we should ensure that the merging jobs are never
        /// cancelled.

        Q_FOREACH (KisRunnableStrokeJobData *job, jobs) {
            job->setCancellable(false);
        }

        runnableJobsInterface()->addRunnableJobs(jobs);
    }
    else {
        KisCommandUtils::CompositeCommand *wrapper = new KisCommandUtils::CompositeCommand(parentCommand.data());
        wrapper->addCommand(m_transaction->endAndTake());
        if (m_closingUndoCommand) {
            wrapper->addCommand(m_closingUndoCommand.release());
        }

        m_transaction.reset();
        deletePainters();

        if (undoAdapter) {
            parentCommand->redo();
            undoAdapter->addCommand(parentCommand);
        }

    }

}

void KisPainterBasedStrokeStrategy::setAdditionalUndoCommand(KUndo2Command *command, KUndo2Command *closingCommand)
{
    m_additionalUndoCommand.reset(command);
    m_closingUndoCommand.reset(closingCommand);
}

void KisPainterBasedStrokeStrategy::setSequentialDabRendering(bool value)
{
    m_sequentialDabRendering = value;
}

void KisPainterBasedStrokeStrategy::cancelStrokeCallback()
{
    m_additionalUndoCommand.reset();
    m_closingUndoCommand.reset();
    if (!m_transaction) return;

    if (m_autokeyCommand) {
        m_autokeyCommand->undo();
    }

    KisNodeSP node = m_resources->currentNode();
    KisIndirectPaintingSupport *indirect =
        dynamic_cast<KisIndirectPaintingSupport*>(node.data());

    bool revert = true;
    if (indirect) {
        KisPaintDeviceSP t = indirect->temporaryTarget();
        if (t) {
            m_transaction.reset();
            deletePainters();

            KisRegion region = t->region();
            for (KisNodeSP target : strokeNodes()) {
                dynamic_cast<KisIndirectPaintingSupport *>(target.data())->setTemporaryTarget(nullptr);
            }
            updateStrokeNodes(region.rects());
            revert = false;
        }
    }

    if (revert) {
        m_transaction->revert();
        m_transaction.reset();
        deletePainters();
    }
}

void KisPainterBasedStrokeStrategy::suspendStrokeCallback()
{
    for (KisNodeSP node : strokeNodes()) {
        KisIndirectPaintingSupport *indirect =
            dynamic_cast<KisIndirectPaintingSupport*>(node.data());

        if(indirect && indirect->hasTemporaryTarget()) {
            m_finalMergeSuspenders << indirect->trySuspendFinalMerge();
            indirect->setTemporaryTarget(nullptr);
        }
    }
}

void KisPainterBasedStrokeStrategy::resumeStrokeCallback()
{
    const bool throughLayers = !m_resources->throughLayerTargets().isEmpty();

    for (KisNodeSP node : strokeNodes()) {
        KisIndirectPaintingSupport *indirect =
            dynamic_cast<KisIndirectPaintingSupport*>(node.data());

        if(indirect) {
            // todo: don't ask about paint device
            // todo:change to an assert
            if (node->paintDevice() != m_targetDevice) {
                if (throughLayers) {
                    attachThroughLayerTarget(node, m_targetDevice);
                } else {
                    indirect->setTemporaryTarget(m_targetDevice);
                    indirect->setTemporaryCompositeOp(m_resources->compositeOpId());
                    indirect->setTemporaryOpacity(m_resources->opacity());
                    indirect->setTemporarySelection(m_activeSelection);

                    QBitArray channelLockFlags = m_resources->channelLockFlags();
                    indirect->setTemporaryChannelFlags(channelLockFlags);
                }
                // Solstice (GPU engine): see initStrokeCallback()
                indirect->setTemporaryTargetPaintedOnCpu(m_maskingBrushRenderer && !m_gpuMasking);
            }
        }
    }

    m_finalMergeSuspenders.clear();
}

KisNodeSP KisPainterBasedStrokeStrategy::targetNode() const
{
    return m_resources->currentNode();
}

KisPainterBasedStrokeStrategy::FakeUndoData::FakeUndoData()
{
    undoStore.reset(new KisDumbUndoStore());
    undoAdapter.reset(new KisPostExecutionUndoAdapter(undoStore.data(), nullptr));
}

KisPainterBasedStrokeStrategy::FakeUndoData::~FakeUndoData()
{
}
