/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUCOVERAGEPASS_H
#define KISGPUCOVERAGEPASS_H

#include <QRect>
#include <QString>
#include <QVector>

#include <memory>

#include "KisGpuTilePool.h"
#include "KisGpuVulkanFunctions.h"
#include "kritagpu_export.h"

class KisGpuBuffer;
class KisGpuCommandList;
class KisGpuComputePipeline;
class KisGpuContext;

/**
 * Applies a coverage mask to tiles in place (shaders/apply_coverage.comp;
 * Solstice: transparency masks). Inside @p clip a pixel with coverage 255
 * stays as it is, one with coverage 0 is cleared, and the others get their
 * alpha multiplied by the coverage, like Krita's Copy composite op copying
 * the pixel through the mask onto a cleared pixel. The rest of every tile is
 * left untouched.
 *
 * The tables live in an internal host-visible buffer: do not record again
 * before the previous recording has completed.
 */
class KRITAGPU_EXPORT KisGpuCoveragePass
{
public:
    /// Larger masks are refused.
    static constexpr qint64 MaxMaskBytes = 4096 * 4096;

    struct Pass {
        QVector<VkDeviceAddress> tiles; ///< row-major over gridWidth columns
        int gridWidth = 0;
        QRect clip; ///< grid pixels
        const quint8 *mask = nullptr; ///< tightly packed coverage bytes; copied before return
        QRect maskBounds; ///< grid pixels
    };

    ~KisGpuCoveragePass();

    static std::unique_ptr<KisGpuCoveragePass>
    create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage = nullptr);

    bool record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage = nullptr);

private:
    explicit KisGpuCoveragePass(KisGpuContext &context);

    KisGpuContext &m_context;
    std::shared_ptr<KisGpuComputePipeline> m_pipeline; // KisGpuContext::sharedComputePipeline
    std::unique_ptr<KisGpuBuffer> m_tables;
};

#endif // KISGPUCOVERAGEPASS_H
