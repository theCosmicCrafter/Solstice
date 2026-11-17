/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISREDRAWABLELAYERINTERFACE_H
#define KISREDRAWABLELAYERINTERFACE_H

#include <QTransform>

#include <functional>

#include "kis_types.h"
#include "kritaimage_export.h"

class KUndo2Command;

/**
 * A paint layer that can draw its content again for a transform instead of
 * resampling its pixels (Solstice: the brush stroke layer, see
 * docs/agent/brush-stroke-layer.md). KisTransformProcessingVisitor asks
 * it before transforming the layer's pixels.
 */
class KRITAIMAGE_EXPORT KisRedrawableLayerInterface
{
public:
    virtual ~KisRedrawableLayerInterface();

    /// @p source transformed as the caller transforms pixels (its filter),
    /// into a new device
    using PixelTransform = std::function<KisPaintDeviceSP(KisPaintDeviceSP source)>;

    /**
     * A command that leaves the layer's paint device as @p transform applied
     * to its content, drawn again, or nullptr when the layer cannot do it
     * (the pixels are transformed as usual then). @p original holds the
     * content before the transform: the paint device itself, or a copy that
     * the Transform Tool keeps while the device shows its preview. Where the
     * content cannot be drawn again, the layer may use @p transformPixels
     * for that part. Called while the image is being processed.
     */
    virtual KUndo2Command *createTransformRedrawCommand(const QTransform &transform,
                                                        KisPaintDeviceSP original,
                                                        const PixelTransform &transformPixels) = 0;
};

#endif // KISREDRAWABLELAYERINTERFACE_H
