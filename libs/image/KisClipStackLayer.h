/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISCLIPSTACKLAYER_H
#define KISCLIPSTACKLAYER_H

#include "kis_layer.h"
#include "kritaimage_export.h"

/**
 * Solstice: the isolated stack of a clip base and the clipped layers above
 * it (Clipping Mask, see docs/agent/clipping-mask-plan.md).
 *
 * The base layer owns one instance. It is not part of the node graph: only
 * the projection leaf tree inserts it in place of the base and its clipped
 * layers. Its original receives the base (Normal, full opacity) and the
 * clipped layers (alpha locked); the stack itself is composited onto the
 * base's parent with the base's blend mode, opacity and channel flags.
 */
class KRITAIMAGE_EXPORT KisClipStackLayer : public KisLayer
{
    Q_OBJECT

public:
    explicit KisClipStackLayer(KisLayer *base);
    ~KisClipStackLayer() override;

    /// The base layer, or null when it has been destroyed
    KisLayerSP base() const;

    KisNodeSP clone() const override;
    bool allowAsChild(KisNodeSP node) const override;

    KisPaintDeviceSP original() const override;
    KisPaintDeviceSP paintDevice() const override;

    /**
     * Converts and clears the device when \p colorSpace, the image's, differs.
     * The stack is outside the node graph, so the image's color space
     * conversion does not visit it. Call it only in an exclusive context.
     */
    void syncColorSpace(const KoColorSpace *colorSpace);

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

typedef KisSharedPtr<KisClipStackLayer> KisClipStackLayerSP;

#endif // KISCLIPSTACKLAYER_H
