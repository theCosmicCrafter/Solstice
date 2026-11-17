/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUPROJECTIONCOMPOSITOR_H
#define KISGPUPROJECTIONCOMPOSITOR_H

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

#include <KisGpuLayerCompositor.h>
#include <KoColor.h>

#include "KisGpuColorTransform.h"
#include "kis_types.h"
#include "kritaimage_export.h"

/**
 * GPU engine phase 2: composites a run of layers onto a projection device on
 * the GPU (docs/agent/gpu-engine.md).
 *
 * The projection's tiles are composited in place on the GPU (phase 3): they
 * become GPU-valid and CPU-stale, and CPU readers download them on demand.
 * Layers are read through their GPU-resident tiles (KisGpuTileAccess,
 * uploaded only when stale). Only pixels inside the rect change.
 *
 * Requires that no other thread writes the same projection tiles meanwhile:
 * KisUpdaterContext does not run merge jobs that share a tile while the GPU
 * projection is enabled.
 *
 * Thread-safe: concurrent calls use separate work contexts.
 */
class KRITAIMAGE_EXPORT KisGpuProjectionCompositor
{
public:
    struct Layer {
        KisPaintDeviceSP device;
        KisGpuBlendOp op = KisGpuBlendOp::Over;
        float opacity = 1.0f;
        bool alphaLocked = false;
        quint32 channelMask = 0xf;
        bool halfBrush = false;
        bool explicitChannelFlags = false;
        /// Copy instead of blending: inside the rect the projection becomes this
        /// device, its default pixel where it has no data
        /// (KisPainter::copyAreaOptimized). The default pixels must be equal;
        /// only leading layers may replace. With a channelMask other than
        /// 0xf it is the Copy composite op at full opacity with channel flags
        /// (phase 4.115): only the enabled channels are replaced.
        bool replace = false;
    };

    /// Maps a Krita composite op id to a GPU blend op; false if unsupported.
    static bool blendOpForCompositeOp(const QString &compositeOpId, KisGpuBlendOp *op);

    /// Test isolation: refuses while a context is leased; waits outside the pool lock.
    static bool resetWorkContextsForTesting();
    static int workContextCountForTesting();
    static int leasedWorkContextCountForTesting();

    /**
     * Composites @p layers (bottom to top) onto @p projection inside @p rect.
     *
     * Every layer device must have the projection's color space (RGBA F32 or
     * F16); its offset may differ from the projection's by any distance
     * (phase 4.112). The projection's own offset must be a multiple of 64
     * pixels. Only tiles inside a layer's extent are read, matching
     * KisLayerProjectionPlane (which clips to the extent).
     *
     * Successful submissions return without waiting. Reusing a busy context
     * may wait when all cached contexts are in flight.
     * Optional coverage is a tightly packed CPU snapshot in image coordinates,
     * copied before return. It supports one RGBA32F layer in a supported blend mode,
     * or one RGBA16F layer in a supported brush mode, including channel locks,
     * and is limited to 16 MiB. Replace layers below it ignore the coverage.
     *
     * @return false on failure; the projection is then unchanged
     */
    static bool composite(KisPaintDeviceSP projection,
                          const QRect &rect,
                          const QVector<Layer> &layers,
                          QString *errorMessage = nullptr,
                          const KisGpuLayerCompositor::Mask *mask = nullptr);

    /**
     * Solstice: Clone Layer offsets (docs/agent/clone-layer.md). Inside
     * @p dstRect, @p dst becomes @p src shifted by @p offset:
     * dst(p) = src(p - offset), the source's default pixel where it has no
     * data, like KisPainter::copyAreaOptimized(dstRect.topLeft(), src, dst,
     * dstRect.translated(-offset)). Pixels are copied unconverted (the CPU
     * Copy op may clear the color of some transparent RGBA F32 pixels).
     *
     * Both devices must have the same RGBA F32/F16 color space and default
     * pixel, and @p dst an offset that is a multiple of 64 (the same
     * exclusivity rule as composite()). Does not wait.
     *
     * @return false when unsupported or on failure; @p dst is then unchanged
     */
    static bool copyShifted(KisPaintDeviceSP dst,
                            const QRect &dstRect,
                            KisPaintDeviceSP src,
                            const QPoint &offset,
                            QString *errorMessage = nullptr);
    /// Successful copyShifted() calls since startup (tests and measurements).
    static quint64 shiftedCopyCount();

    /**
     * Solstice: transparency masks (docs/agent/gpu-engine.md, phase 4.113).
     * Applies @p coverage (tightly packed bytes of @p rect, in image
     * coordinates) to @p device in place, as the Copy composite op does
     * when KisMask::mergeInMaskInternal() copies the pixels through the
     * mask's selection onto the cleared projection: coverage 255 keeps a
     * pixel, 0 clears it, anything between multiplies its alpha.
     *
     * The device must be RGBA F32/F16 with a transparent default pixel and
     * an offset that is a multiple of 64 (the same exclusivity rule as
     * composite()); @p rect at most 4096 x 4096 pixels. Only the device's
     * existing tiles are touched. Does not wait.
     *
     * @return false when unsupported or on failure; @p device is then unchanged
     */
    static bool
    applyCoverage(KisPaintDeviceSP device, const QRect &rect, const quint8 *coverage, QString *errorMessage = nullptr);
    /// Successful applyCoverage() calls since startup (tests and measurements).
    static quint64 coverageCount();

    /**
     * Solstice: Stroke layer style (docs/agent/gpu-engine.md, phase 4.114).
     * A layer with an outside, solid color Stroke style, as
     * KisLayerStyleProjectionPlane::apply() composites it.
     */
    struct StrokeStyle {
        KisPaintDeviceSP layer; ///< the layer's projection
        KisGpuBlendOp layerOp = KisGpuBlendOp::Over;
        float layerOpacity = 1.0f;
        bool layerAlphaLocked = false;
        quint32 layerChannelMask = 0xf;

        KoColor strokeColor; ///< in the projection's color space
        KisGpuBlendOp strokeOp = KisGpuBlendOp::Over;
        float strokeOpacity = 1.0f;
        bool strokeAlphaLocked = false;
        quint32 strokeChannelMask = 0xf;
        qreal strokeSize = 1.0; ///< pixels; at most KisGpuStrokeStylePass::MaxRadius
    };

    /// Whether compositeStrokeStyle() can run at all (GPU with 64-bit float shaders).
    static bool supportsStrokeStyle();
    /// Blend modes compositeStrokeStyle() accepts for the layer and the stroke.
    static bool strokeStyleSupportsBlendOp(KisGpuBlendOp op);
    /// Largest stroke size compositeStrokeStyle() accepts.
    static int maxStrokeStyleSize();

    /**
     * Composites the layer and its stroke onto @p projection inside
     * @p rect: the layer with its alpha maxed out, and the stroke color
     * where the dilated layer alpha reaches outside the layer, copied
     * through that coverage like the CPU's knockout.
     *
     * The projection and the layer must share an RGBA F32/F16 color space,
     * the layer needs a transparent default pixel, and the projection an
     * offset that is a multiple of 64. Does not wait.
     *
     * @return false when unsupported or on failure; the projection is then unchanged
     */
    static bool compositeStrokeStyle(KisPaintDeviceSP projection,
                                     const QRect &rect,
                                     const StrokeStyle &style,
                                     QString *errorMessage = nullptr);
    /// Successful compositeStrokeStyle() calls since startup (tests and measurements).
    static quint64 strokeStyleCount();

    /// Whether transformColors() can run at all (GPU with 64-bit float shaders).
    static bool supportsColorTransform();
    /// Largest number of pixels of a transformColors() rect with a coverage.
    static qint64 maxColorTransformCoveragePixels();

    /**
     * Solstice: filter layers (docs/agent/gpu-engine.md, phase 4.115).
     * Inside @p rect, @p dst becomes the transformed @p src (its default
     * pixel where it has no data), as KisUpdateOriginalVisitor filters the
     * projection into an adjustment layer's original device.
     *
     * With @p coverage (tightly packed bytes of @p rect, the layer's
     * selection) the transformed pixel is copied onto the source pixel
     * through it with the Copy composite op: 255 gives the transformed
     * pixel, 0 the source pixel.
     *
     * Both devices must share an RGBA F32/F16 color space (tables: F16
     * only), and @p dst needs an offset that is a multiple of 64 (the same
     * exclusivity rule as composite()). @p src may be @p dst (a filter mask
     * on its layer's projection, phase 4.116); tiles the device lacks
     * inside @p rect are then created from its default pixel, as the CPU
     * filter does. Does not wait.
     *
     * @return false when unsupported or on failure; @p dst is then unchanged
     */
    static bool transformColors(KisPaintDeviceSP dst,
                                const QRect &rect,
                                KisPaintDeviceSP src,
                                const KisGpuColorTransform &transform,
                                const quint8 *coverage = nullptr,
                                QString *errorMessage = nullptr);
    /// Successful transformColors() calls since startup (tests and measurements).
    static quint64 colorTransformCount();
};

#endif // KISGPUPROJECTIONCOMPOSITOR_H
