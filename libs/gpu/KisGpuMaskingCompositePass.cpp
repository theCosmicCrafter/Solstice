/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuMaskingCompositePass.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <cstring>

namespace
{
const quint32 MaskingRgba32f[] = {
#include "masking_composite_rgba32f.spv.inc"
};
const quint32 MaskingRgba16f[] = {
#include "masking_composite_rgba16f.spv.inc"
};

struct Params {
    qint32 rect[4];
    qint32 gridOrigin[2];
    qint32 gridWidth;
    qint32 gridHeight;
    qint32 mode;
    qint32 padding[3];
};
static_assert(sizeof(Params) == 48, "must match the shader Params block");

struct PushConstants {
    VkDeviceAddress params;
    VkDeviceAddress strokeTiles;
    VkDeviceAddress dstTiles;
    VkDeviceAddress mask;
    VkDeviceAddress uint8ToFloat;
};

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

KisGpuMaskingCompositePass::KisGpuMaskingCompositePass(KisGpuContext &context)
    : m_context(context)
{
}

KisGpuMaskingCompositePass::~KisGpuMaskingCompositePass() = default;

std::unique_ptr<KisGpuMaskingCompositePass> KisGpuMaskingCompositePass::create(KisGpuContext &context,
                                                                               QString *errorMessage)
{
    if (!context.deviceInfo().supportsFloat64) {
        if (errorMessage)
            *errorMessage = QStringLiteral("the GPU masking composite requires shaderFloat64");
        return nullptr;
    }
    std::unique_ptr<KisGpuMaskingCompositePass> pass(new KisGpuMaskingCompositePass(context));
    pass->m_pipeline =
        context.sharedComputePipeline(MaskingRgba32f, sizeof(MaskingRgba32f), sizeof(PushConstants), errorMessage);
    return pass->m_pipeline ? std::move(pass) : nullptr;
}

bool KisGpuMaskingCompositePass::record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    const int tileCount = pass.gridWidth * pass.gridHeight;
    if (pass.rect.isEmpty() || pass.mode < 0 || pass.mode >= ModeCount || tileCount <= 0
        || pass.strokeTiles.size() != tileCount || pass.dstTiles.size() != tileCount
        || pass.mask.size() != pass.rect.width() * pass.rect.height())
        return fail(QStringLiteral("invalid masking composite pass"));
    const QRect grid(pass.gridOrigin, QSize(pass.gridWidth * 64, pass.gridHeight * 64));
    if (!grid.contains(pass.rect))
        return fail(QStringLiteral("masking composite tiles do not cover the rect"));
    if (pass.half && !m_halfPipeline) {
        m_halfPipeline = m_context.sharedComputePipeline(MaskingRgba16f,
                                                         sizeof(MaskingRgba16f),
                                                         sizeof(PushConstants),
                                                         errorMessage);
        if (!m_halfPipeline)
            return false;
    }

    const VkDeviceSize paramsBytes = alignUp(sizeof(Params), 64);
    const VkDeviceSize tilesBytes = alignUp(VkDeviceSize(tileCount) * sizeof(VkDeviceAddress), 64);
    const VkDeviceSize strokeOffset = paramsBytes;
    const VkDeviceSize dstOffset = strokeOffset + tilesBytes;
    const VkDeviceSize lutOffset = dstOffset + tilesBytes;
    const VkDeviceSize maskOffset = lutOffset + alignUp(256 * sizeof(float), 64);
    const VkDeviceSize totalBytes = maskOffset + alignUp(VkDeviceSize(pass.mask.size()), 64);
    if (!m_tables || m_tables->size() < totalBytes) {
        m_tables.reset();
        m_tables = KisGpuBuffer::create(m_context, totalBytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables)
            return false;
    }

    quint8 *mapped = static_cast<quint8 *>(m_tables->mapped());
    Params params{};
    params.rect[0] = pass.rect.x();
    params.rect[1] = pass.rect.y();
    params.rect[2] = pass.rect.width();
    params.rect[3] = pass.rect.height();
    params.gridOrigin[0] = pass.gridOrigin.x();
    params.gridOrigin[1] = pass.gridOrigin.y();
    params.gridWidth = pass.gridWidth;
    params.gridHeight = pass.gridHeight;
    params.mode = pass.mode;
    std::memcpy(mapped, &params, sizeof(params));
    std::memcpy(mapped + strokeOffset, pass.strokeTiles.constData(), size_t(tileCount) * sizeof(VkDeviceAddress));
    std::memcpy(mapped + dstOffset, pass.dstTiles.constData(), size_t(tileCount) * sizeof(VkDeviceAddress));
    std::memcpy(mapped + lutOffset, pass.uint8ToFloat, sizeof(pass.uint8ToFloat));
    std::memcpy(mapped + maskOffset, pass.mask.constData(), size_t(pass.mask.size()));

    const VkDeviceAddress base = m_tables->deviceAddress();
    const PushConstants constants{base, base + strokeOffset, base + dstOffset, base + maskOffset, base + lutOffset};
    commands.computeBarrier();
    (pass.half ? m_halfPipeline : m_pipeline)->dispatch(commands.commandBuffer(), constants, quint32(tileCount));
    commands.computeBarrier();
    return true;
}
