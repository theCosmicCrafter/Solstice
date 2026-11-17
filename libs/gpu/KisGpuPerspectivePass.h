/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUPERSPECTIVEPASS_H
#define KISGPUPERSPECTIVEPASS_H

#include <QPoint>
#include <QRectF>
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
 * The backward-map pass of KisPerspectiveTransformWorker on the GPU
 * (shaders/perspective_transform.comp): the 3x3 projective backward map of
 * QTransform::do_map() per destination pixel inside the recorded row spans,
 * then bilinear (KisRandomSubAccessor::sampledOldRawData()) or
 * nearest-neighbour sampling, bit-identical to the CPU worker.
 *
 * The pass covers the whole destination tile grid: pixels inside the row
 * spans with a source point inside the clip rect are sampled and written;
 * every other pixel is written the destination's default pixel
 * (OverwriteAll, for runImpl() on the cleared device) or keeps the content
 * the destination access uploaded (KeepUnwritten, for runPartialDst()).
 *
 * A sample that needs a pixel outside the source tile grid, or a non-finite
 * source point, sets the failure flag; check failed() after the
 * submission's wait and discard the result then (run the CPU path).
 *
 * Requires shaderFloat64. Tables live in an internal host-visible buffer:
 * do not record again before the previous recording has completed.
 */
class KRITAGPU_EXPORT KisGpuPerspectivePass
{
public:
    /// Pixels the shader does not sample.
    enum Mode {
        OverwriteAll = 0, ///< write the destination's default pixel
        KeepUnwritten = 1, ///< keep the uploaded destination content
    };

    struct Pass {
        double backward[9] = {}; ///< dst -> src, as m11 m21 m31 m12 m22 m32 m13 m23 m33
        QRectF srcClipRect; ///< the CPU checks srcClipRect.contains(srcPoint)
        Mode mode = OverwriteAll;
        bool nearest = false;
        int regionTop = 0; ///< first row of the row table (image coordinates)
        QVector<qint32> rows; ///< per row: span offset, span count into spans
        QVector<qint32> spans; ///< per span: xStart, xEnd (exclusive)
        QPoint srcGridOrigin; ///< image coordinates of the first source tile
        int srcGridWidth = 0;
        int srcGridHeight = 0;
        QVector<VkDeviceAddress> srcTiles; ///< row-major; 0 reads the default pixel
        QPoint dstGridOrigin;
        int dstGridWidth = 0;
        int dstGridHeight = 0;
        QVector<VkDeviceAddress> dstTiles;
        float defaultPixel[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< source default, tile memory order
        float clearPixel[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< destination default, tile memory order
    };

    ~KisGpuPerspectivePass();

    /// nullptr without shaderFloat64 or on pipeline failure.
    static std::unique_ptr<KisGpuPerspectivePass>
    create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage = nullptr);

    bool record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage = nullptr);

    /// After the recording's submission completed: a sample fell outside the
    /// source tile grid, so the result must not be used.
    bool failed() const;

private:
    explicit KisGpuPerspectivePass(KisGpuContext &context);

    KisGpuContext &m_context;
    std::shared_ptr<KisGpuComputePipeline> m_pipeline;
    std::unique_ptr<KisGpuBuffer> m_tables;
    std::unique_ptr<KisGpuBuffer> m_flag;
};

#endif // KISGPUPERSPECTIVEPASS_H
