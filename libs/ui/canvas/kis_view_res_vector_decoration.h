/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VIEW_RES_VECTOR_DECORATION_H
#define KIS_VIEW_RES_VECTOR_DECORATION_H

#include <QPointer>
#include <QRect>

#include "kis_canvas_decoration.h"

class KisView;

/**
 * \brief Canvas decoration that re-composites eligible vector layers at
 *        view resolution (fix for audit defect D7).
 *
 * See KisViewResVectorCompositor and docs/agent/view-res-vectors.md for the
 * architecture. This decoration owns the per-canvas state: the unit/job
 * analysis, the document-resolution backdrop and raster-unit caches, the
 * view-resolution vector patches and the dirty tracking fed by
 * KisImage::sigImageUpdated.
 *
 * It is added to every view canvas and engages only when
 * KisConfig::viewResVectorRendering() is on, the canvas shows a single
 * tile (no wrap-around), and the image is at LOD 0. Otherwise it paints
 * nothing and the canvas shows the normal document-resolution projection.
 */
class KisViewResVectorDecoration : public KisCanvasDecoration
{
    Q_OBJECT

public:
    static const QString idTag;

    explicit KisViewResVectorDecoration(QPointer<KisView> parent);
    ~KisViewResVectorDecoration() override;

protected:
    void drawDecoration(QPainter &gc,
                        const QRectF &updateArea,
                        const KisCoordinatesConverter *converter,
                        KisCanvas2 *canvas) override;

private Q_SLOTS:
    void slotImageUpdated(const QRect &rc);

private:
    struct Private;
    Private *const m_d;
};

#endif // KIS_VIEW_RES_VECTOR_DECORATION_H
