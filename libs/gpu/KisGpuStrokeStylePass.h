/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUSTROKESTYLEPASS_H
#define KISGPUSTROKESTYLEPASS_H

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

#include "KisGpuLayerCompositor.h"
#include "KisGpuTilePool.h"
#include "KisGpuVulkanFunctions.h"
#include "kritagpu_export.h"

class KisGpuBuffer;
class KisGpuCommandList;
class KisGpuComputePipeline;
class KisGpuContext;

/**
 * Composites one layer with a Stroke layer style (outside, solid color)
 * onto destination tiles (shaders/composite_stroke_style.comp; Solstice:
 * GPU engine phase 4.114). The layer is blended with its alpha maxed out,
 * and the stroke color replaces the result where the dilated layer alpha
 * reaches outside the layer, exactly as the CPU layer style does.
 *
 * Needs 64-bit float shaders (the dilation is summed in double). Basic
 * blend modes only (below KisGpuBlendOp::SoftLightSvg, not Erase).
 *
 * The tables live in an internal host-visible buffer: do not record again
 * before the previous recording has completed.
 */
class KRITAGPU_EXPORT KisGpuStrokeStylePass
{
public:
    /// Largest dilation radius in pixels.
    static constexpr int MaxRadius = 16;

    struct Pass {
        QVector<VkDeviceAddress> dstTiles; ///< row-major over gridWidth columns
        int gridWidth = 0;
        QRect clip; ///< grid pixels

        /// The layer's tiles around the clip rect, grown by the radius;
        /// row-major over layerTableSize. 0: no tile (transparent).
        QVector<VkDeviceAddress> layerTiles;
        QSize layerTableSize;
        QPoint layerBias; ///< grid pixel + bias = pixel in the layer's table

        KisGpuBlendOp layerOp = KisGpuBlendOp::Over;
        float layerOpacity = 1.0f;
        bool layerAlphaLocked = false;
        quint32 layerChannelMask = 0xf;

        float strokeColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; ///< as float, tile memory order
        KisGpuBlendOp strokeOp = KisGpuBlendOp::Over;
        float strokeOpacity = 1.0f;
        bool strokeAlphaLocked = false;
        quint32 strokeChannelMask = 0xf;

        int radius = 1; ///< 1..MaxRadius
        QVector<double> kernel; ///< (2 * radius + 1)^2 weights, row-major
    };

    ~KisGpuStrokeStylePass();

    static bool supportsBlendOp(KisGpuBlendOp op);

    static std::unique_ptr<KisGpuStrokeStylePass>
    create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage = nullptr);

    bool record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage = nullptr);

private:
    explicit KisGpuStrokeStylePass(KisGpuContext &context);

    KisGpuContext &m_context;
    std::shared_ptr<KisGpuComputePipeline> m_pipeline; // KisGpuContext::sharedComputePipeline
    std::unique_ptr<KisGpuBuffer> m_tables;
};

#endif // KISGPUSTROKESTYLEPASS_H
