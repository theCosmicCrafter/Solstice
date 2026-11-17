/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUSHIFTEDCOPYPASS_H
#define KISGPUSHIFTEDCOPYPASS_H

#include <QPoint>
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
 * Copies pixels between two tile grids with any pixel shift
 * (shaders/copy_shifted.comp; Solstice: Clone Layer offsets).
 *
 * Every destination pixel inside @p clip (destination grid pixels) becomes
 * the source pixel at the same position plus @p shift (source grid pixels),
 * or @p fill where the source tile address is 0 or outside the source grid.
 * Pixels are copied without conversion; the rest of every destination tile
 * is left untouched.
 *
 * The address tables live in an internal host-visible buffer: do not record
 * again before the previous recording has completed.
 */
class KRITAGPU_EXPORT KisGpuShiftedCopyPass
{
public:
    struct Copy {
        QVector<VkDeviceAddress> dstTiles; ///< row-major over dstGridWidth columns
        int dstGridWidth = 0;
        QVector<VkDeviceAddress> srcTiles; ///< row-major; 0 reads @p fill
        int srcGridWidth = 0;
        int srcGridHeight = 0;
        QPoint shift;
        QRect clip;
        float fill[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< as float, tile memory order
    };

    ~KisGpuShiftedCopyPass();

    static std::unique_ptr<KisGpuShiftedCopyPass>
    create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage = nullptr);

    bool record(KisGpuCommandList &commands, const Copy &copy, QString *errorMessage = nullptr);

private:
    explicit KisGpuShiftedCopyPass(KisGpuContext &context);

    KisGpuContext &m_context;
    std::shared_ptr<KisGpuComputePipeline> m_pipeline; // KisGpuContext::sharedComputePipeline
    std::unique_ptr<KisGpuBuffer> m_tables;
};

#endif // KISGPUSHIFTEDCOPYPASS_H
