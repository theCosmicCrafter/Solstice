/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISBRUSHSTROKELAYER_H
#define KISBRUSHSTROKELAYER_H

#include <QAtomicInt>
#include <QMutex>
#include <QPoint>
#include <QSharedPointer>
#include <QTransform>
#include <QVector>

#include <KisRedrawableLayerInterface.h>
#include <KoColor.h>
#include <kis_paint_information.h>
#include <kis_paint_layer.h>
#include <kis_timing_information.h>
#include <kis_types.h>
#include <kundo2command.h>
#include <resources/KoAbstractGradient.h>
#include <resources/KoPattern.h>

#include "kritaui_export.h"

class QIODevice;

/**
 * One brush stroke as the brush tool drew it on a KisBrushStrokeLayer: the
 * stroke's jobs and the state they were drawn with, enough to draw it again
 * at another resolution. See docs/agent/brush-stroke-layer.md.
 */
struct KRITAUI_EXPORT KisRecordedBrushStroke {
    /// A FreehandStrokeStrategy::Data job of type POINT, LINE or CURVE
    struct Job {
        int type = 0;
        int strokeInfoId = 0;
        KisPaintInformation pi1;
        KisPaintInformation pi2;
        QPointF control1;
        QPointF control2;
    };

    KisPaintOpPresetSP preset;
    KoColor fgColor;
    KoColor bgColor;
    QString compositeOpId;
    qreal opacity = 1.0;
    qreal effectiveZoom = 1.0;
    KoPatternSP pattern;
    KoAbstractGradientSP gradient;
    int seed = 0;
    /// the number of mirrored copies the tool painted (KisFreehandStrokeInfo)
    int strokeInfoCount = 1;
    /// the offset of the layer's paint device when the stroke was drawn
    QPoint deviceOffset;
    /// the canvas mirror (Mirror Horizontally/Vertically): the painter
    /// mirrors every dab around @p mirrorAxesCenter, in image pixels
    bool mirrorHorizontal = false;
    bool mirrorVertical = false;
    QPointF mirrorAxesCenter;

    /// Where the dab spacing of each mirrored copy started: the brush tool
    /// starts it from the cursor's last hover position and direction
    /// (KisToolFreehandHelper), which places every dab of the stroke
    struct Start {
        bool hasLastDab = false;
        QPointF lastPosition;
        qreal lastAngle = 0.0;
        qreal spacingUpdateInterval = LONG_TIME;
        qreal timingUpdateInterval = LONG_TIME;
        int dabSeqNo = 0;
    };
    QVector<Start> starts;
    QVector<Job> jobs;
};

using KisRecordedBrushStrokeSP = QSharedPointer<const KisRecordedBrushStroke>;

/**
 * A paint layer that keeps the brush strokes drawn on it, so that they can
 * be drawn again at a new resolution (Solstice). Only the brush tool paints
 * on it. See docs/agent/brush-stroke-layer.md.
 */
class KRITAUI_EXPORT KisBrushStrokeLayer : public KisPaintLayer, public KisRedrawableLayerInterface
{
    Q_OBJECT
public:
    KisBrushStrokeLayer(KisImageWSP image, const QString &name, quint8 opacity, const KoColorSpace *colorSpace);
    KisBrushStrokeLayer(const KisBrushStrokeLayer &rhs);
    ~KisBrushStrokeLayer() override;

    KisNodeSP clone() const override;
    QIcon icon() const override;

    QVector<KisRecordedBrushStrokeSP> strokes() const;
    void addStroke(KisRecordedBrushStrokeSP stroke);
    void removeStroke(KisRecordedBrushStrokeSP stroke);
    void setStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes);

    /// Whether the pixels are still just the recorded strokes, as the last
    /// background check found (KisBrushStrokeLayerMonitor)
    enum class RecordState {
        Unknown,
        Matching,
        NotMatching,
    };
    RecordState recordState() const;

    /// Changes the pixels; an edit other than a recorded brush stroke makes
    /// the layer check its record in the background once the image is idle
    using KisPaintLayer::setDirty;
    void setDirty(const QVector<QRect> &rects) override;

    /// The next check may be skipped: the change since the last one was a
    /// recorded brush stroke or a redraw, which keep the pixels and the
    /// record together, unless another change came with it
    void markChangeRecorded();

    /// Pixel changes between these calls (a recorded brush stroke drawing,
    /// its undo and redo, a redraw) belong to a recorded change; any other
    /// change makes the next check run (KisRecordedChangeScopeCommand)
    void beginRecordedChange();
    void endRecordedChange();

    /// A change inside a recorded scope that the record does not hold (a
    /// stroke that turned out not to be recordable): the next check runs
    void markChangeUnrecorded();

    /**
     * The record after the Move Tool moved the pixels inside @p mask (the
     * selection where it was before the move) by @p offset: the strokes that
     * lie inside the selection move with the pixels and come last, as the
     * moved pixels are pasted over the others. nullptr if nothing moves or
     * a stroke crosses the selection's edge (the cut splits it, so the
     * record no longer matches the pixels).
     */
    KUndo2Command *createSelectionMoveCommand(KisPaintDeviceSP mask, const QPoint &offset);

    /**
     * The record after the Move Tool duplicated the pixels inside @p mask
     * (the selection where it was before the drag) and placed the copy
     * @p offset away: the strokes that lie inside the selection stay, and
     * copies of them, moved by @p offset, come last, as the copy is pasted
     * over everything. nullptr if nothing is copied or a stroke crosses the
     * selection's edge (the copy holds only part of it).
     */
    KUndo2Command *createSelectionDuplicateCommand(KisPaintDeviceSP mask, const QPoint &offset);

    /// Whether drawing @p strokes again gives the pixels of @p original;
    /// otherwise @p difference describes how they differ
    static bool strokesMatchPixels(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                   KisPaintDeviceSP original,
                                   QString *difference);

    /**
     * Draws the strokes again with @p transform applied (scaling and moving
     * only) into the paint device, replacing what it holds. Where @p
     * original (the content before the transform) is not the recorded
     * strokes (another tool or a filter changed it, a selection clipped a
     * stroke), that part keeps its pixels, transformed by @p
     * transformPixels and blended in at the edge. nullptr when most of the
     * content is not the record, or for rotation and mirroring: the pixels
     * are transformed as on a paint layer.
     */
    KUndo2Command *createTransformRedrawCommand(const QTransform &transform,
                                                KisPaintDeviceSP original,
                                                const PixelTransform &transformPixels) override;

    /// @p strokes with their positions mapped by @p transform from the
    /// device at @p deviceOffset, and their brush sizes scaled; their
    /// recorded offset becomes @p deviceOffset
    static QVector<KisRecordedBrushStrokeSP> transformedStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                                                const QPoint &deviceOffset,
                                                                const QTransform &transform);

    /**
     * Draws the recorded strokes again at the current resolution into the
     * paint device, replacing what it holds: the layer is exactly its record
     * again, so anything that is not a recorded stroke (a filter's result,
     * the cut part of a stroke clipped by a selection) is removed. The
     * pixels change, the record does not; the background check then takes
     * down the out-of-sync warning. nullptr when there is no record or the
     * layer is animated.
     */
    KUndo2Command *createRerenderCommand();

    /**
     * Drops the recorded strokes, keeping the pixels: the layer stays a
     * brush stroke layer and records new strokes, but until then it scales
     * its pixels like a paint layer. nullptr when there is no record.
     */
    KUndo2Command *createForgetStrokesCommand();

    /**
     * Draws @p strokes again into a new paint device in @p colorSpace, through
     * the brush tool's stroke path with each stroke's seed. The positions are
     * moved by the difference between @p deviceOffset and each stroke's
     * recorded offset, then mapped by @p transform; the brush sizes are
     * scaled by the transform's scale. Blocks until done.
     */
    static KisPaintDeviceSP renderStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                          const KoColorSpace *colorSpace,
                                          const QRect &imageBounds,
                                          const QPoint &deviceOffset,
                                          const QTransform &transform = QTransform());

    /**
     * Writes @p strokes to @p device in Solstice's brush stroke format (the
     * layer's file in a .kra, KisBrushStrokeLayerIO.cpp). Brush tips,
     * textures, patterns and gradients are embedded when their data can be
     * saved; ABR tip images are stored as PNGs.
     */
    static bool saveStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes, QIODevice *device);

    /// Reads strokes written by saveStrokes(); false for damaged or newer
    /// data. Strokes whose brush engine is missing are left out.
    static bool loadStrokes(QIODevice *device, QVector<KisRecordedBrushStrokeSP> *strokes);

private:
    friend class KisBrushStrokeLayerMonitor;
    /// Asks for a background check of the record (any thread)
    void requestRecordCheck();
    void setRecordState(RecordState state, const QString &difference);

    mutable QMutex m_mutex;
    QVector<KisRecordedBrushStrokeSP> m_strokes;
    /// counts changes to the pixels and the record
    QAtomicInt m_generation;
    /// a check is requested and not yet started
    QAtomicInt m_checkRequested;
    /// since the last check: a recorded change, and a change outside one
    QAtomicInt m_changeRecorded;
    QAtomicInt m_unrecordedChange;
    /// open recorded scopes (beginRecordedChange())
    QAtomicInt m_recordedChanges;
    QAtomicInt m_recordState{int(RecordState::Unknown)};
};

using KisBrushStrokeLayerSP = KisSharedPtr<KisBrushStrokeLayer>;

/// Adds a recorded stroke to its layer on redo and removes it on undo; part
/// of the stroke's undo command
class KRITAUI_EXPORT KisRecordBrushStrokeCommand : public KUndo2Command
{
public:
    KisRecordBrushStrokeCommand(KisBrushStrokeLayer *layer, KisRecordedBrushStrokeSP stroke);
    void redo() override;
    void undo() override;

private:
    KisBrushStrokeLayerSP m_layer;
    KisRecordedBrushStrokeSP m_stroke;
};

/**
 * Brackets a recorded change in its undo command: an Opening command goes
 * first and a Closing one last, so that the pixel changes of the commands
 * between them happen inside KisBrushStrokeLayer::beginRecordedChange() and
 * endRecordedChange(), in redo and in undo (which runs the commands in
 * reverse). With @p scopeOpen, the stroke that makes the command has opened
 * the scope already: the first redo of the Opening command does nothing,
 * and the first redo of the Closing command (or its destruction without
 * one) closes it.
 */
class KRITAUI_EXPORT KisRecordedChangeScopeCommand : public KUndo2Command
{
public:
    enum Position {
        Opening,
        Closing,
    };
    KisRecordedChangeScopeCommand(KisBrushStrokeLayer *layer, Position position, bool scopeOpen = false);
    ~KisRecordedChangeScopeCommand() override;
    void redo() override;
    void undo() override;

private:
    KisBrushStrokeLayerSP m_layer;
    Position m_position;
    bool m_scopeOpen;
};

#endif // KISBRUSHSTROKELAYER_H
