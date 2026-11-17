/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_PAINTER_BASED_STROKE_STRATEGY_H
#define __KIS_PAINTER_BASED_STROKE_STRATEGY_H

#include <QVector>

#include "KisRunnableBasedStrokeStrategy.h"
#include "kis_resources_snapshot.h"
#include "kis_selection.h"
#include "kis_indirect_painting_support.h"

class KisPainter;
class KisDistanceInformation;
class KisTransaction;
class KisFreehandStrokeInfo;
class KisMaskedFreehandStrokePainter;
class KisMaskingBrushRenderer;
class KisRunnableStrokeJobData;
class KisRunnableStrokeJobsInterface;
class KisUndoStore;

class KRITAUI_EXPORT KisPainterBasedStrokeStrategy : public KisRunnableBasedStrokeStrategy
{
public:
    KisPainterBasedStrokeStrategy(const QLatin1String &id,
                                  const KUndo2MagicString &name,
                                  KisResourcesSnapshotSP resources,
                                  QVector<KisFreehandStrokeInfo*> strokeInfos);

    KisPainterBasedStrokeStrategy(const QLatin1String &id,
                                  const KUndo2MagicString &name,
                                  KisResourcesSnapshotSP resources,
                                  KisFreehandStrokeInfo *strokeInfo);

    ~KisPainterBasedStrokeStrategy();

    void initStrokeCallback() override;
    void finishStrokeCallback() override;
    void cancelStrokeCallback() override;

    void suspendStrokeCallback() override;
    void resumeStrokeCallback() override;

    /**
     * Solstice: the paint ops render their dabs one after another instead of
     * on several threads, so that the stroke's pixels do not depend on the
     * threads' timing and the same stroke can be drawn again exactly (the
     * brush stroke layer, docs/agent/brush-stroke-layer.md). Call it
     * before the stroke starts.
     */
    void setSequentialDabRendering(bool value);

protected:
    KisNodeSP targetNode() const;
    KisPaintDeviceSP targetDevice() const;
    KisSelectionSP activeSelection() const;

    KisMaskedFreehandStrokePainter* maskedPainter(int strokeInfoId);
    int numMaskedPainters() const;

    void setUndoEnabled(bool value);

    /**
     * Return true if the descendant should execute a few more jobs before issuing setDirty()
     * call on the layer.
     *
     * If the returned value is true, then the stroke actually paints **not** on the
     * layer's paint device, but on some intermediate device owned by
     * KisPainterBasedStrokeStrategy and one should merge it first before asking the
     * update.
     *
     * The value can be true only when the stroke is declared to support masked brush!
     * \see supportsMaskingBrush()
     */
    bool needsMaskingUpdates() const;

    /**
     * Create a list of update jobs that should be run before issuing the setDirty()
     * call on the node
     *
     * \see needsMaskingUpdates()
     */
    QVector<KisRunnableStrokeJobData*> doMaskingBrushUpdates(const QVector<QRect> &rects);

    /// Solstice: updates the projection of what the stroke painted in @p rects
    void updateStrokeNodes(const QVector<QRect> &rects);

protected:

    /**
     * The descendants may declare if this stroke should support auto-creation
     * of the masked brush. Default value: false
     */
    void setSupportsMaskingBrush(bool value);

    /**
     * Return if the stroke should auto-create a masked brush from the provided
     * paintop preset or not
     */
    bool supportsMaskingBrush() const;

    void setSupportsIndirectPainting(bool value);
    bool supportsIndirectPainting() const;

    bool supportsContinuedInterstrokeData() const;
    void setSupportsContinuedInterstrokeData(bool value);

    bool supportsTimedMergeId() const;
    void setSupportsTimedMergeId(bool value);

protected:
    KisPainterBasedStrokeStrategy(const KisPainterBasedStrokeStrategy &rhs, int levelOfDetail);

    /// Solstice: @p command becomes part of the stroke's undo command, done
    /// and undone with the painting (e.g. recording the stroke on a brush
    /// stroke layer); it comes first, and @p closingCommand, if any, comes
    /// after the painting's commands. Call it before finishStrokeCallback().
    void setAdditionalUndoCommand(KUndo2Command *command, KUndo2Command *closingCommand = nullptr);

private:
    void init();
    void initPainters(KisPaintDeviceSP targetDevice, KisPaintDeviceSP maskingDevice,
                      KisSelectionSP selection,
                      bool hasIndirectPainting,
                      const QString &indirectPaintingCompositeOp);
    void deletePainters();
    /// Solstice: every node the stroke composites onto
    KisNodeList strokeNodes() const;
    /// Solstice: @p node composites the shared device of a through-layer
    /// erase with its own selection and channel locks
    void attachThroughLayerTarget(KisNodeSP node, KisPaintDeviceSP device);
    inline int timedID(const QString &id){
        return int(qHash(id));
    }

private:
    KisResourcesSnapshotSP m_resources;
    QVector<KisFreehandStrokeInfo*> m_strokeInfos;
    QVector<KisFreehandStrokeInfo*> m_maskStrokeInfos;
    QVector<KisMaskedFreehandStrokePainter*> m_maskedPainters;

    QScopedPointer<KisTransaction> m_transaction;

    QScopedPointer<KisMaskingBrushRenderer> m_maskingBrushRenderer;
    // Solstice (GPU engine): the masking composite runs on the GPU
    // (KisGpuMaskingWorker), decided once per stroke like the brush op's path
    bool m_gpuMasking = false;
    // Solstice: a Build-up through-layer erase accumulates its dabs in the
    // shared device at the stroke's opacity, as they would erase one by one
    bool m_throughLayerBuildUp = false;

    KisPaintDeviceSP m_targetDevice;
    KisSelectionSP m_activeSelection;

    std::unique_ptr<KUndo2Command> m_autokeyCommand;
    std::unique_ptr<KUndo2Command> m_additionalUndoCommand;
    std::unique_ptr<KUndo2Command> m_closingUndoCommand;
    bool m_sequentialDabRendering{false};
    QScopedPointer<KisRunnableStrokeJobsInterface> m_sequentialJobs;

    bool m_useMergeID {false};

    bool m_supportsMaskingBrush {false};
    bool m_supportsIndirectPainting {false};
    bool m_supportsContinuedInterstrokeData {false};

    QVector<KisIndirectPaintingSupport::FinalMergeSuspenderSP> m_finalMergeSuspenders;

    struct FakeUndoData {
        FakeUndoData();
        ~FakeUndoData();
        QScopedPointer<KisUndoStore> undoStore;
        QScopedPointer<KisPostExecutionUndoAdapter> undoAdapter;
    };
    QScopedPointer<FakeUndoData> m_fakeUndoData;

};

#endif /* __KIS_PAINTER_BASED_STROKE_STRATEGY_H */
