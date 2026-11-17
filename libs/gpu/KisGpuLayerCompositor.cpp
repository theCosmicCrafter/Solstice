/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuLayerCompositor.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <algorithm>
#include <cstring>

#include <kis_debug.h>

namespace
{
const quint32 CompositeLayersRgba32f[] = {
#include "composite_layers_rgba32f.spv.inc"
};

const quint32 CompositeLayersRgba16f[] = {
#include "composite_layers_rgba16f.spv.inc"
};
const quint32 CompositeLayersExtendedRgba32f[] = {
#include "composite_layers_extended_rgba32f.spv.inc"
};
const quint32 CompositeLayersExtendedRgba16f[] = {
#include "composite_layers_extended_rgba16f.spv.inc"
};

struct PushConstants {
    VkDeviceAddress layerTiles;
    VkDeviceAddress dstTiles;
    VkDeviceAddress layers;
    quint32 tileCount;
    quint32 layerCount;
    quint32 gridWidth;
    quint32 padding[3];
    qint32 clip[4];
    VkDeviceAddress mask;
    qint32 maskOrigin[2];
    qint32 maskSize[2];
    quint32 maskPadding[2];
};
static_assert(sizeof(PushConstants) == 96, "must match the shader push constant block");

struct LayerParams {
    float opacity;
    quint32 op;
    quint32 flags;
    quint32 channelMask;
    float fill[4]; // replace layers: the pixel where the layer has no tile
    qint32 bias[2]; // added to a grid pixel position: the pixel in the layer's table
    quint32 tableOffset; // the layer's first tile in the address table
    quint32 tableWidth; // tiles per row of the layer's table
};
static_assert(sizeof(LayerParams) == 48, "must match the shader LayerParams struct");

constexpr int TileSize = 64;

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

KisGpuLayerCompositor::KisGpuLayerCompositor(KisGpuContext &context)
    : m_context(context)
{
}

KisGpuLayerCompositor::~KisGpuLayerCompositor() = default;

std::unique_ptr<KisGpuLayerCompositor>
KisGpuLayerCompositor::create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage)
{
    std::unique_ptr<KisGpuLayerCompositor> compositor(new KisGpuLayerCompositor(context));
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    compositor->m_f16 = f16;
    compositor->m_pipeline =
        context.sharedComputePipeline(f16 ? CompositeLayersRgba16f : CompositeLayersRgba32f,
                                      f16 ? sizeof(CompositeLayersRgba16f) : sizeof(CompositeLayersRgba32f),
                                      sizeof(PushConstants),
                                      errorMessage);
    return compositor->m_pipeline ? std::move(compositor) : nullptr;
}

bool KisGpuLayerCompositor::preparePipelines(KisGpuContext &context,
                                             KisGpuTileFormat format,
                                             bool extended,
                                             QString *errorMessage)
{
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    if (!context.sharedComputePipeline(f16 ? CompositeLayersRgba16f : CompositeLayersRgba32f,
                                       f16 ? sizeof(CompositeLayersRgba16f) : sizeof(CompositeLayersRgba32f),
                                       sizeof(PushConstants),
                                       errorMessage))
        return false;
    return !extended
        || context.sharedComputePipeline(f16 ? CompositeLayersExtendedRgba16f : CompositeLayersExtendedRgba32f,
                                         f16 ? sizeof(CompositeLayersExtendedRgba16f)
                                             : sizeof(CompositeLayersExtendedRgba32f),
                                         sizeof(PushConstants),
                                         errorMessage);
}

bool KisGpuLayerCompositor::record(KisGpuCommandList &commands,
                                   const QVector<VkDeviceAddress> &layerTiles,
                                   const QVector<VkDeviceAddress> &dstTiles,
                                   const QVector<Layer> &layers,
                                   int gridWidth,
                                   const QRect &clip,
                                   QString *errorMessage,
                                   const Mask *mask)
{
    const int tileCount = dstTiles.size();
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(gridWidth > 0 && tileCount % gridWidth == 0, false);
    const int gridHeight = tileCount / gridWidth;
    // Every layer's table: the destination grid, or one more column and row
    // for a shifted layer.
    QVector<quint32> tableOffsets;
    tableOffsets.reserve(layers.size());
    qint64 tableEntries = 0;
    for (const Layer &layer : layers) {
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(layer.shift.x() >= 0 && layer.shift.x() < TileSize && layer.shift.y() >= 0
                                                 && layer.shift.y() < TileSize,
                                             false);
        tableOffsets << quint32(tableEntries);
        tableEntries += layer.shift.isNull() ? qint64(tileCount) : qint64(gridWidth + 1) * (gridHeight + 1);
    }
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(layerTiles.size() == tableEntries, false);
    if (!tileCount || layers.isEmpty() || clip.isEmpty()) {
        return true;
    }

    const VkDeviceSize layerTableBytes = VkDeviceSize(layerTiles.size()) * sizeof(VkDeviceAddress);
    const VkDeviceSize dstTableOffset = alignUp(layerTableBytes, 16);
    const VkDeviceSize dstTableBytes = VkDeviceSize(tileCount) * sizeof(VkDeviceAddress);
    const VkDeviceSize paramsOffset = alignUp(dstTableOffset + dstTableBytes, 16);
    const VkDeviceSize maskOffset = paramsOffset + VkDeviceSize(layers.size()) * sizeof(LayerParams);
    const qint64 maskBytes = mask ? qint64(mask->bounds.width()) * mask->bounds.height() : 0;
    // Coverage applies to one blended layer, optionally above replace layers.
    const bool singleBlendedLayer =
        !layers.last().replace && std::all_of(layers.cbegin(), layers.cend() - 1, [](const Layer &layer) {
            return layer.replace;
        });
    if (mask && (!mask->data || mask->bounds.isEmpty() || maskBytes > 4096 * 4096 || !singleBlendedLayer)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("unsupported or oversized layer coverage mask");
        return false;
    }
    const VkDeviceSize totalBytes = maskOffset + alignUp(VkDeviceSize(maskBytes), 4);
    bool extended = false;
    for (const auto &layer : layers) {
        if (layer.halfBrush && (!m_f16 || (layer.op != KisGpuBlendOp::Over && layer.op != KisGpuBlendOp::Erase))) {
            if (errorMessage)
                *errorMessage = QStringLiteral("half brush arithmetic requires F16 Normal or Erase");
            return false;
        }
        if (layer.channelMask > 0xf || quint32(layer.op) >= quint32(KisGpuBlendOp::Count)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("unsupported layer channel mask");
            return false;
        }
        extended |= layer.op >= KisGpuBlendOp::SoftLightSvg;
    }

    // Compile the extra rounding constraints separately so they cannot affect
    // established modes through shared-expression optimization.
    if (extended && !m_extendedPipeline) {
        m_extendedPipeline = m_context.sharedComputePipeline(
            m_f16 ? CompositeLayersExtendedRgba16f : CompositeLayersExtendedRgba32f,
            m_f16 ? sizeof(CompositeLayersExtendedRgba16f) : sizeof(CompositeLayersExtendedRgba32f),
            sizeof(PushConstants),
            errorMessage);
        if (!m_extendedPipeline)
            return false;
    }

    if (!m_tables || m_tables->size() < totalBytes) {
        m_tables = KisGpuBuffer::create(m_context, totalBytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables) {
            return false;
        }
    }

    quint8 *mapped = static_cast<quint8 *>(m_tables->mapped());
    std::memcpy(mapped, layerTiles.constData(), size_t(layerTableBytes));
    std::memcpy(mapped + dstTableOffset, dstTiles.constData(), size_t(dstTableBytes));
    LayerParams *params = reinterpret_cast<LayerParams *>(mapped + paramsOffset);
    for (int i = 0; i < layers.size(); i++) {
        params[i].opacity = layers[i].opacity;
        params[i].op = quint32(layers[i].op);
        params[i].flags = (layers[i].alphaLocked ? 1u : 0u) | (layers[i].halfBrush ? 2u : 0u)
            | (layers[i].explicitChannelFlags ? 4u : 0u) | (layers[i].replace ? 8u : 0u);
        params[i].channelMask = layers[i].channelMask;
        std::memcpy(params[i].fill, layers[i].fill, sizeof(params[i].fill));
        const bool shifted = !layers[i].shift.isNull();
        params[i].bias[0] = shifted ? TileSize - layers[i].shift.x() : 0;
        params[i].bias[1] = shifted ? TileSize - layers[i].shift.y() : 0;
        params[i].tableOffset = tableOffsets[i];
        params[i].tableWidth = quint32(shifted ? gridWidth + 1 : gridWidth);
    }

    PushConstants constants{};
    constants.layerTiles = m_tables->deviceAddress();
    constants.dstTiles = m_tables->deviceAddress() + dstTableOffset;
    constants.layers = m_tables->deviceAddress() + paramsOffset;
    constants.tileCount = quint32(tileCount);
    constants.layerCount = quint32(layers.size());
    constants.gridWidth = quint32(gridWidth);
    constants.clip[0] = clip.left();
    constants.clip[1] = clip.top();
    constants.clip[2] = clip.right() + 1;
    constants.clip[3] = clip.bottom() + 1;
    if (mask) {
        std::memset(mapped + maskOffset, 0, size_t(alignUp(VkDeviceSize(maskBytes), 4)));
        std::memcpy(mapped + maskOffset, mask->data, size_t(maskBytes));
        constants.mask = m_tables->deviceAddress() + maskOffset;
        constants.maskOrigin[0] = mask->bounds.x();
        constants.maskOrigin[1] = mask->bounds.y();
        constants.maskSize[0] = mask->bounds.width();
        constants.maskSize[1] = mask->bounds.height();
    }

    // A mixed stack must also keep old modes out of the extended shader.
    // Consecutive runs share the tables and submission; only their layers and
    // pipeline change (a layer finds its tiles through its table offset). Each
    // run observes the preceding run's destination writes.
    for (int first = 0; first < layers.size();) {
        const bool useExtended = layers[first].op >= KisGpuBlendOp::SoftLightSvg;
        int end = first + 1;
        while (end < layers.size() && (layers[end].op >= KisGpuBlendOp::SoftLightSvg) == useExtended)
            ++end;
        constants.layers = m_tables->deviceAddress() + paramsOffset + VkDeviceSize(first) * sizeof(LayerParams);
        constants.layerCount = quint32(end - first);
        if (first)
            commands.computeBarrier();
        (useExtended ? m_extendedPipeline : m_pipeline)
            ->dispatch(commands.commandBuffer(), constants, quint32(tileCount));
        first = end;
    }
    return true;
}
