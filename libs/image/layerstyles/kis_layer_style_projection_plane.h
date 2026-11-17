/*
 *  SPDX-FileCopyrightText: 2015 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_LAYER_STYLE_PROJECTION_PLANE_H
#define __KIS_LAYER_STYLE_PROJECTION_PLANE_H

#include "kis_abstract_projection_plane.h"

#include <QScopedPointer>

#include "kis_types.h"

#include <kritaimage_export.h>


class KRITAIMAGE_EXPORT KisLayerStyleProjectionPlane : public KisAbstractProjectionPlane
{
public:
    KisLayerStyleProjectionPlane(KisLayer *sourceLayer);
    KisLayerStyleProjectionPlane(const KisLayerStyleProjectionPlane &rhs, KisLayer *sourceLayer, KisPSDLayerStyleSP clonedStyle);

    ~KisLayerStyleProjectionPlane() override;

    QRect recalculate(const QRect& rect, KisNodeSP filthyNode, KisRenderPassFlags flags) override;
    void apply(KisPainter *painter, const QRect &rect) override;

    QRect needRect(const QRect &rect, KisLayer::PositionToFilthy pos) const override;
    QRect changeRect(const QRect &rect, KisLayer::PositionToFilthy pos) const override;
    QRect accessRect(const QRect &rect, KisLayer::PositionToFilthy pos) const override;
    QRect needRectForOriginal(const QRect &rect) const override;
    QRect tightUserVisibleBounds() const override;
    QRect looseUserVisibleBounds() const override;

    KisPaintDeviceList getLodCapableDevices() const override;


    // a method for registering on KisLayerStyleProjectionPlaneFactory
    static KisAbstractProjectionPlaneSP factoryObject(KisLayer *sourceLayer);

    /**
     * Solstice (GPU engine, phase 4.114): true when the GPU engine
     * composites this layer together with its style: the only enabled
     * effect is a Stroke outside the layer with a solid color, a size and
     * blend modes the GPU pass supports, in an image the GPU composites,
     * at full resolution. recalculate() then leaves the style's CPU
     * devices alone, and the merger calls applyOnGpu() instead of apply().
     */
    bool gpuCompositesStyles() const;

    /**
     * Composites the layer and its stroke onto @p projection on the GPU.
     * @return false when the GPU could not do it; the projection is then
     * unchanged and the caller uses apply(), which computes the style on
     * the CPU first.
     */
    bool applyOnGpu(KisPaintDeviceSP projection, const QRect &rect);

private:
    friend class KisLayerStyleProjectionPlaneTest;
    KisLayerStyleProjectionPlane(KisLayer *sourceLayer, KisPSDLayerStyleSP style);

    void init(KisLayer *sourceLayer, KisPSDLayerStyleSP layerStyle);

    QRect stylesNeedRect(const QRect &rect) const;

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

typedef QSharedPointer<KisLayerStyleProjectionPlane> KisLayerStyleProjectionPlaneSP;
typedef QWeakPointer<KisLayerStyleProjectionPlane> KisLayerStyleProjectionPlaneWSP;

#endif /* __KIS_LAYER_STYLE_PROJECTION_PLANE_H */
