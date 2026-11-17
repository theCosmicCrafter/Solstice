/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushStrokeLayer.h"

#include <QMutexLocker>

#include <kis_default_bounds.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_paint_device.h>

KisBrushStrokeLayer::KisBrushStrokeLayer(KisImageWSP image,
                                         const QString &name,
                                         quint8 opacity,
                                         const KoColorSpace *colorSpace)
    : KisPaintLayer(image, name, opacity, colorSpace)
{
}

KisBrushStrokeLayer::KisBrushStrokeLayer(const KisBrushStrokeLayer &rhs)
    : KisPaintLayer(rhs)
    , m_strokes(rhs.strokes())
{
}

KisBrushStrokeLayer::~KisBrushStrokeLayer()
{
}

KisNodeSP KisBrushStrokeLayer::clone() const
{
    return KisNodeSP(new KisBrushStrokeLayer(*this));
}

QIcon KisBrushStrokeLayer::icon() const
{
    // the Layers docker draws this small in the thumbnail's corner, so a
    // badge on the brush would not show: the warning replaces the brush
    // (scaling will scale the pixels)
    return KisIconUtils::loadIcon(recordState() == RecordState::NotMatching ? "warning" : "krita_tool_freehand");
}

QVector<KisRecordedBrushStrokeSP> KisBrushStrokeLayer::strokes() const
{
    QMutexLocker locker(&m_mutex);
    return m_strokes;
}

void KisBrushStrokeLayer::addStroke(KisRecordedBrushStrokeSP stroke)
{
    QMutexLocker locker(&m_mutex);
    m_strokes.append(stroke);
}

void KisBrushStrokeLayer::removeStroke(KisRecordedBrushStrokeSP stroke)
{
    QMutexLocker locker(&m_mutex);
    m_strokes.removeOne(stroke);
}

void KisBrushStrokeLayer::setStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes)
{
    {
        QMutexLocker locker(&m_mutex);
        m_strokes = strokes;
    }
    m_generation.ref();
    // a loaded or replaced record is checked against the pixels
    requestRecordCheck();
}

KisBrushStrokeLayer::RecordState KisBrushStrokeLayer::recordState() const
{
    return RecordState(m_recordState.loadAcquire());
}

void KisBrushStrokeLayer::setDirty(const QVector<QRect> &rects)
{
    // a level of detail preview (Instant Preview) changes only its own
    // copy of the pixels; the full resolution change follows
    KisPaintDeviceSP device = paintDevice();
    if (!device || !device->defaultBounds()->currentLevelOfDetail()) {
        m_generation.ref();
        if (!m_recordedChanges.loadAcquire()) {
            m_unrecordedChange.storeRelease(1);
        }
        requestRecordCheck();
    }
    KisPaintLayer::setDirty(rects);
}

void KisBrushStrokeLayer::markChangeRecorded()
{
    m_generation.ref();
    m_changeRecorded.storeRelease(1);
}

void KisBrushStrokeLayer::beginRecordedChange()
{
    m_recordedChanges.ref();
}

void KisBrushStrokeLayer::endRecordedChange()
{
    m_recordedChanges.deref();
}

void KisBrushStrokeLayer::markChangeUnrecorded()
{
    m_generation.ref();
    m_unrecordedChange.storeRelease(1);
    requestRecordCheck();
}

KisRecordBrushStrokeCommand::KisRecordBrushStrokeCommand(KisBrushStrokeLayer *layer, KisRecordedBrushStrokeSP stroke)
    : m_layer(layer)
    , m_stroke(stroke)
{
}

void KisRecordBrushStrokeCommand::redo()
{
    m_layer->addStroke(m_stroke);
    m_layer->markChangeRecorded();
}

void KisRecordBrushStrokeCommand::undo()
{
    m_layer->removeStroke(m_stroke);
    m_layer->markChangeRecorded();
}

KisRecordedChangeScopeCommand::KisRecordedChangeScopeCommand(KisBrushStrokeLayer *layer,
                                                             Position position,
                                                             bool scopeOpen)
    : m_layer(layer)
    , m_position(position)
    , m_scopeOpen(scopeOpen)
{
}

KisRecordedChangeScopeCommand::~KisRecordedChangeScopeCommand()
{
    // never redone: the stroke's scope must not stay open
    if (m_scopeOpen && m_position == Closing) {
        m_layer->endRecordedChange();
    }
}

void KisRecordedChangeScopeCommand::redo()
{
    if (m_scopeOpen) {
        m_scopeOpen = false;
        if (m_position == Opening) {
            return;
        }
    } else if (m_position == Opening) {
        m_layer->beginRecordedChange();
        return;
    }
    m_layer->endRecordedChange();
}

void KisRecordedChangeScopeCommand::undo()
{
    // undo runs the commands in reverse: Closing comes first
    if (m_position == Closing) {
        m_layer->beginRecordedChange();
    } else {
        m_layer->endRecordedChange();
    }
}
