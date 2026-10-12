/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUMASKINGCOMPOSITEPASS_H
#define KISGPUMASKINGCOMPOSITEPASS_H

#include <QByteArray>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

#include <memory>

#include "KisGpuVulkanFunctions.h"
#include "kritagpu_export.h"

class KisGpuBuffer;
class KisGpuCommandList;
class KisGpuComputePipeline;
class KisGpuContext;

/**
 * The masking brush composite on RGBA32F or RGBA16F (phase 4.106) tiles
 * (shaders/masking_composite.comp):
 * inside the rect, each destination pixel becomes the stroke pixel with its
 * alpha composited with the mask, bit-identical to KisMaskingBrushRenderer's
 * CPU composite. See docs/agent/gpu-engine.md ("Masking brush").
 *
 * Requires shaderFloat64. The tables live in an internal buffer: do not
 * record again before the previous recording has completed.
 */
class KRITAGPU_EXPORT KisGpuMaskingCompositePass
{
public:
    struct Pass {
        QRect rect;
        /// KisMaskingBrushCompositeFuncTypes (0-9, the modes without strength)
        int mode = 0;
        QPoint gridOrigin; ///< image coordinates of the first tile
        int gridWidth = 0;
        int gridHeight = 0;
        QVector<VkDeviceAddress> strokeTiles; ///< row-major; 0 reads a transparent pixel
        QVector<VkDeviceAddress> dstTiles; ///< the same grid
        QByteArray mask; ///< gray * alpha of the mask, rect.width() * rect.height() bytes
        /// KoLuts::Uint8ToFloat; RGBA16F: KoColorSpaceMaths<quint8, half> as floats
        float uint8ToFloat[256] = {};
        bool half = false; ///< RGBA16F tiles
    };

    ~KisGpuMaskingCompositePass();

    /// nullptr without shaderFloat64 or on pipeline failure.
    static std::unique_ptr<KisGpuMaskingCompositePass> create(KisGpuContext &context, QString *errorMessage = nullptr);

    bool record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage = nullptr);

    /// the number of composite modes the shader supports
    static constexpr int ModeCount = 10;

private:
    explicit KisGpuMaskingCompositePass(KisGpuContext &context);

    KisGpuContext &m_context;
    std::shared_ptr<KisGpuComputePipeline> m_pipeline; // KisGpuContext::sharedComputePipeline
    std::shared_ptr<KisGpuComputePipeline> m_halfPipeline; // RGBA16F, created on first use
    std::unique_ptr<KisGpuBuffer> m_tables;
};

#endif // KISGPUMASKINGCOMPOSITEPASS_H
