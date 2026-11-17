/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUPERSPECTIVEWORKER_H
#define KISGPUPERSPECTIVEWORKER_H

#include <QRect>
#include <QRectF>
#include <QTransform>
#include <QVector>

#include "kis_types.h"
#include "kritaimage_export.h"

/**
 * GPU engine (Solstice): the per-pixel backward projective map of
 * KisPerspectiveTransformWorker on the GPU, bit-identical to the CPU
 * (docs/agent/gpu-engine.md).
 *
 * The worker covers both call sites of the CPU worker:
 *  - runImpl() (the FREE_TRANSFORM and PERSPECTIVE_4POINT legs of the
 *    Transform Tool): the source is a clone of the cleared destination,
 *    and pixels without a sample are written the destination's default
 *    pixel — Mode::OverwriteAll;
 *  - runPartialDst() (affine updates of a transform mask, one rect of the
 *    destination at a time): pixels without a sample keep their content —
 *    Mode::KeepUnwritten.
 *
 * On false the destination is either untouched or holds partial results the
 * caller repaints on the CPU (mode semantics are preserved either way).
 */
class KRITAIMAGE_EXPORT KisGpuPerspectiveWorker
{
public:
    /// What a destination pixel without a source sample gets.
    enum Mode {
        OverwriteAll = 0, ///< the destination's default pixel (runImpl())
        KeepUnwritten = 1, ///< the existing destination content (runPartialDst())
    };

    /**
     * The destination region as per-row spans. rowSpans holds, per row
     * (regionTop + index), pairs of (offset, count) into spans; spans holds
     * [xStart, xEnd) pairs. bounds is the bounding rect; it sets the
     * destination tile grid.
     */
    struct Region {
        QRect bounds;
        QVector<qint32> rows; ///< 2 per row of bounds.height(): offset, count
        QVector<qint32> spans; ///< 2 per span: xStart, xEnd (exclusive)
    };

    /**
     * Cheap checks: engine and switches, RGBA F32/F16 devices with the same
     * color space and pixel size, LOD 0, no wrap-around, shaderFloat64.
     */
    static bool canRun(KisPaintDeviceSP src, KisPaintDeviceSP dst);

    /**
     * For every pixel inside @p region: map dst -> src through @p backward
     * (QTransform::map()) and, when the source point is inside
     * @p srcClipRect (QRectF::contains()), sample @p src bilinear
     * (@p nearest false) or nearest-neighbour (@p nearest true) and write
     * the result into @p dst. Pixels without a sample follow @p mode.
     * The reads are the source's current tiles, like the other GPU workers;
     * the callers pass sources whose old data equals it.
     * Thread: the caller owns both devices' tiles.
     */
    static bool run(KisPaintDeviceSP src,
                    KisPaintDeviceSP dst,
                    const QTransform &backward,
                    const QRectF &srcClipRect,
                    const Region &region,
                    Mode mode,
                    bool nearest);

    /// Enabled with the GPU engine; KRITA_GPU_TRANSFORM=0 or
    /// KRITA_GPU_PERSPECTIVE=0 disables it.
    static bool isEnabled();
    /// Number of perspective transforms that ran on the GPU (tests, diagnostics).
    static quint64 runCount();
};

#endif // KISGPUPERSPECTIVEWORKER_H
