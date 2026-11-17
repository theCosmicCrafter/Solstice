/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUCOLORTRANSFORMPASS_H
#define KISGPUCOLORTRANSFORMPASS_H

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
 * Applies a per-pixel color transformation while copying between two tile
 * grids (shaders/color_transform.comp; Solstice: filter layers).
 *
 * Every destination pixel inside @p clip becomes the transformed source
 * pixel at the same position plus @p shift (the fill color where the source
 * has no tile). With a coverage mask the transformed pixel is copied onto
 * the source pixel through it, like Krita's Copy composite op with a
 * selection: 255 writes the transformed pixel, 0 the source pixel. The rest
 * of every destination tile is left untouched.
 *
 * The tables live in internal host-visible buffers: do not record again
 * before the previous recording has completed.
 */
class KRITAGPU_EXPORT KisGpuColorTransformPass
{
public:
    /// Larger masks are refused.
    static constexpr qint64 MaxMaskBytes = 4096 * 4096;
    /// Entries of one channel table, and the size of the four tables together.
    static constexpr int ChannelTableSize = 65536;
    static constexpr qint64 ChannelTablesBytes = 4 * ChannelTableSize * qint64(sizeof(quint16));
    /// Larger table blocks are refused.
    static constexpr qint64 MaxTableBytes = 16 * 1024 * 1024;

    /// The shader's transformation kinds.
    enum Kind : quint32 {
        /// RGBA F16 only: one table per channel, indexed by the half bits.
        ChannelTables = 0,
        /// KisDesaturateAdjustment type 0; types 1 to 5 follow.
        Desaturate = 1,
        DesaturateLast = 6,
        /// RGBA F16 only: the Color Reference filter; the tables are its
        /// block of 32-bit words (shaders/color_transform.comp).
        ColorReference = 7
    };

    struct Pass {
        QVector<VkDeviceAddress> dstTiles; ///< row-major over dstGridWidth columns
        int dstGridWidth = 0;
        QVector<VkDeviceAddress> srcTiles; ///< row-major; 0 reads @p fill
        int srcGridWidth = 0;
        int srcGridHeight = 0;
        QPoint shift;
        QRect clip; ///< destination grid pixels
        float fill[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< as float, tile memory order

        quint32 kind = ChannelTables;
        /// ChannelTables: 4 * ChannelTableSize half bit patterns (R, G, B, A
        /// tables). ColorReference: its block. Copied before return.
        const void *tables = nullptr;
        qint64 tableBytes = 0;
        /// Identifies the tables' content: they are uploaded again only when it changes.
        quint64 tablesId = 0;

        const quint8 *mask = nullptr; ///< tightly packed coverage bytes or null; copied before return
        QRect maskBounds; ///< destination grid pixels
    };

    ~KisGpuColorTransformPass();

    static std::unique_ptr<KisGpuColorTransformPass>
    create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage = nullptr);

    bool record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage = nullptr);

private:
    KisGpuColorTransformPass(KisGpuContext &context, bool f16);

    KisGpuContext &m_context;
    const bool m_f16;
    std::shared_ptr<KisGpuComputePipeline> m_pipeline; // KisGpuContext::sharedComputePipeline
    std::unique_ptr<KisGpuBuffer> m_tables; // tile addresses and the coverage mask
    std::unique_ptr<KisGpuBuffer> m_channelTables;
    quint64 m_channelTablesId = 0;
};

#endif // KISGPUCOLORTRANSFORMPASS_H
