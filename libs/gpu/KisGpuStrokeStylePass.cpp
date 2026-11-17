/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuStrokeStylePass.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <cstring>

namespace
{
const quint32 CompositeStrokeStyleRgba32f[] = {
#include "composite_stroke_style_rgba32f.spv.inc"
};

const quint32 CompositeStrokeStyleRgba16f[] = {
#include "composite_stroke_style_rgba16f.spv.inc"
};

struct PushConstants {
    VkDeviceAddress dstTiles;
    VkDeviceAddress layerTiles;
    VkDeviceAddress kernel;
    quint32 tileCount;
    quint32 gridWidth;
    qint32 clip[4];
    qint32 layerBias[2];
    qint32 layerTableSize[2];
    float strokeColor[4];
    float layerOpacity;
    quint32 layerOp;
    quint32 layerAlphaLocked;
    quint32 layerChannelMask;
    float strokeOpacity;
    quint32 strokeOp;
    quint32 strokeAlphaLocked;
    quint32 strokeChannelMask;
    qint32 radius;
    qint32 padding[3];
};
static_assert(sizeof(PushConstants) == 128, "must match the shader push constant block");

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

KisGpuStrokeStylePass::KisGpuStrokeStylePass(KisGpuContext &context)
    : m_context(context)
{
}

KisGpuStrokeStylePass::~KisGpuStrokeStylePass() = default;

bool KisGpuStrokeStylePass::supportsBlendOp(KisGpuBlendOp op)
{
    // The shader is compiled with the basic blend modes only.
    return op < KisGpuBlendOp::SoftLightSvg && op != KisGpuBlendOp::Erase;
}

std::unique_ptr<KisGpuStrokeStylePass>
KisGpuStrokeStylePass::create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage)
{
    std::unique_ptr<KisGpuStrokeStylePass> pass(new KisGpuStrokeStylePass(context));
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    pass->m_pipeline =
        context.sharedComputePipeline(f16 ? CompositeStrokeStyleRgba16f : CompositeStrokeStyleRgba32f,
                                      f16 ? sizeof(CompositeStrokeStyleRgba16f) : sizeof(CompositeStrokeStyleRgba32f),
                                      sizeof(PushConstants),
                                      errorMessage);
    return pass->m_pipeline ? std::move(pass) : nullptr;
}

bool KisGpuStrokeStylePass::record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage)
{
    if (pass.dstTiles.isEmpty() || pass.clip.isEmpty()) {
        return true;
    }
    const int kernelSize = 2 * pass.radius + 1;
    if (pass.gridWidth <= 0 || pass.radius < 1 || pass.radius > MaxRadius
        || pass.kernel.size() != kernelSize * kernelSize || pass.layerTableSize.width() < 0
        || pass.layerTableSize.height() < 0
        || pass.layerTiles.size() != pass.layerTableSize.width() * pass.layerTableSize.height()
        || !supportsBlendOp(pass.layerOp) || !supportsBlendOp(pass.strokeOp) || pass.layerChannelMask > 0xf
        || pass.strokeChannelMask > 0xf) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid stroke style pass");
        }
        return false;
    }

    // One table: destination addresses, layer addresses (at least one entry,
    // so the table address is always valid), then the kernel.
    const int layerEntries = qMax(1, pass.layerTiles.size());
    const VkDeviceSize dstBytes = VkDeviceSize(pass.dstTiles.size()) * sizeof(VkDeviceAddress);
    const VkDeviceSize layerBytes = VkDeviceSize(layerEntries) * sizeof(VkDeviceAddress);
    const VkDeviceSize kernelOffset = alignUp(dstBytes + layerBytes, 16);
    const VkDeviceSize kernelBytes = VkDeviceSize(pass.kernel.size()) * sizeof(double);
    const VkDeviceSize bytes = kernelOffset + kernelBytes;
    if (!m_tables || m_tables->size() < bytes) {
        m_tables = KisGpuBuffer::create(m_context, bytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables) {
            return false;
        }
    }
    quint8 *table = static_cast<quint8 *>(m_tables->mapped());
    std::memcpy(table, pass.dstTiles.constData(), size_t(dstBytes));
    if (pass.layerTiles.isEmpty()) {
        std::memset(table + dstBytes, 0, sizeof(VkDeviceAddress));
    } else {
        std::memcpy(table + dstBytes, pass.layerTiles.constData(), size_t(layerBytes));
    }
    std::memcpy(table + kernelOffset, pass.kernel.constData(), size_t(kernelBytes));

    PushConstants constants{};
    constants.dstTiles = m_tables->deviceAddress();
    constants.layerTiles = m_tables->deviceAddress() + dstBytes;
    constants.kernel = m_tables->deviceAddress() + kernelOffset;
    constants.tileCount = quint32(pass.dstTiles.size());
    constants.gridWidth = quint32(pass.gridWidth);
    constants.clip[0] = pass.clip.left();
    constants.clip[1] = pass.clip.top();
    constants.clip[2] = pass.clip.right() + 1;
    constants.clip[3] = pass.clip.bottom() + 1;
    constants.layerBias[0] = pass.layerBias.x();
    constants.layerBias[1] = pass.layerBias.y();
    constants.layerTableSize[0] = pass.layerTableSize.width();
    constants.layerTableSize[1] = pass.layerTableSize.height();
    std::memcpy(constants.strokeColor, pass.strokeColor, sizeof(constants.strokeColor));
    constants.layerOpacity = pass.layerOpacity;
    constants.layerOp = quint32(pass.layerOp);
    constants.layerAlphaLocked = pass.layerAlphaLocked ? 1 : 0;
    constants.layerChannelMask = pass.layerChannelMask;
    constants.strokeOpacity = pass.strokeOpacity;
    constants.strokeOp = quint32(pass.strokeOp);
    constants.strokeAlphaLocked = pass.strokeAlphaLocked ? 1 : 0;
    constants.strokeChannelMask = pass.strokeChannelMask;
    constants.radius = pass.radius;
    m_pipeline->dispatch(commands.commandBuffer(), constants, quint32(pass.dstTiles.size()));
    return true;
}
