/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuPerspectivePass.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <cstring>

namespace
{
const quint32 PerspectiveTransformRgba32f[] = {
#include "perspective_transform_rgba32f.spv.inc"
};
const quint32 PerspectiveTransformRgba16f[] = {
#include "perspective_transform_rgba16f.spv.inc"
};

struct Params {
    double clipRect[4]; // left, top, right, bottom
    double backward[9];
    qint32 padding0[2]; // vec4 alignment of defaultPixel (std430)
    float defaultPixel[4];
    float clearPixel[4];
    qint32 dstGrid[4]; // origin.x, origin.y, width, height
    qint32 srcGrid[4];
    qint32 regionTop;
    qint32 rowCount;
    qint32 mode;
    qint32 nearest;
};
static_assert(sizeof(Params) == 192, "must match the shader Params block");

struct PushConstants {
    VkDeviceAddress params;
    VkDeviceAddress srcTiles;
    VkDeviceAddress dstTiles;
    VkDeviceAddress rows;
    VkDeviceAddress spans;
    VkDeviceAddress flag;
};

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

KisGpuPerspectivePass::KisGpuPerspectivePass(KisGpuContext &context)
    : m_context(context)
{
}

KisGpuPerspectivePass::~KisGpuPerspectivePass() = default;

std::unique_ptr<KisGpuPerspectivePass>
KisGpuPerspectivePass::create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage)
{
    if (!context.deviceInfo().supportsFloat64) {
        if (errorMessage)
            *errorMessage = QStringLiteral("GPU perspective transforms require shaderFloat64");
        return nullptr;
    }
    std::unique_ptr<KisGpuPerspectivePass> pass(new KisGpuPerspectivePass(context));
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    pass->m_pipeline =
        context.sharedComputePipeline(f16 ? PerspectiveTransformRgba16f : PerspectiveTransformRgba32f,
                                      f16 ? sizeof(PerspectiveTransformRgba16f) : sizeof(PerspectiveTransformRgba32f),
                                      sizeof(PushConstants),
                                      errorMessage);
    pass->m_flag = KisGpuBuffer::create(context, 16, KisGpuBuffer::Location::Readback, errorMessage);
    if (!pass->m_pipeline || !pass->m_flag)
        return nullptr;
    return pass;
}

bool KisGpuPerspectivePass::record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    const int rowCount = pass.rows.size() / 2;
    if (pass.dstGridWidth <= 0 || pass.dstGridHeight <= 0 || pass.srcGridWidth < 0 || pass.srcGridHeight < 0
        || pass.dstTiles.size() != pass.dstGridWidth * pass.dstGridHeight
        || pass.srcTiles.size() != pass.srcGridWidth * pass.srcGridHeight || pass.rows.size() != rowCount * 2
        || rowCount <= 0 || pass.spans.isEmpty() || pass.spans.size() % 2 != 0)
        return fail(QStringLiteral("invalid perspective transform pass"));
    for (int i = 0; i < rowCount; ++i) {
        const qint64 spanIndex = pass.rows[2 * i];
        const qint64 spanCount = pass.rows[2 * i + 1];
        if (spanIndex < 0 || spanCount < 0 || 2 * (spanIndex + spanCount) > pass.spans.size())
            return fail(QStringLiteral("invalid perspective transform row table"));
    }
    if (pass.dstGridWidth > 65535 || qint64(pass.dstGridHeight) * 64 > 65535)
        return fail(QStringLiteral("perspective transform destination too large"));

    const VkDeviceSize paramsBytes = alignUp(sizeof(Params), 32);
    const VkDeviceSize srcOffset = paramsBytes;
    const VkDeviceSize srcBytes = alignUp(VkDeviceSize(qMax(1, pass.srcTiles.size())) * sizeof(VkDeviceAddress), 32);
    const VkDeviceSize dstOffset = srcOffset + srcBytes;
    const VkDeviceSize dstBytes = alignUp(VkDeviceSize(pass.dstTiles.size()) * sizeof(VkDeviceAddress), 32);
    const VkDeviceSize rowsOffset = dstOffset + dstBytes;
    const VkDeviceSize rowsBytes = alignUp(VkDeviceSize(pass.rows.size()) * sizeof(qint32), 32);
    const VkDeviceSize spansOffset = rowsOffset + rowsBytes;
    const VkDeviceSize spansBytes = alignUp(VkDeviceSize(pass.spans.size()) * sizeof(qint32), 32);
    const VkDeviceSize totalBytes = spansOffset + spansBytes;
    if (!m_tables || m_tables->size() < totalBytes) {
        m_tables.reset();
        m_tables = KisGpuBuffer::create(m_context, totalBytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables)
            return false;
    }

    quint8 *mapped = static_cast<quint8 *>(m_tables->mapped());
    Params params{};
    const QRectF clip = pass.srcClipRect.normalized();
    params.clipRect[0] = clip.left();
    params.clipRect[1] = clip.top();
    params.clipRect[2] = clip.left() + clip.width();
    params.clipRect[3] = clip.top() + clip.height();
    std::memcpy(params.backward, pass.backward, sizeof(params.backward));
    params.dstGrid[0] = pass.dstGridOrigin.x();
    params.dstGrid[1] = pass.dstGridOrigin.y();
    params.dstGrid[2] = pass.dstGridWidth;
    params.dstGrid[3] = pass.dstGridHeight;
    params.srcGrid[0] = pass.srcGridOrigin.x();
    params.srcGrid[1] = pass.srcGridOrigin.y();
    params.srcGrid[2] = pass.srcGridWidth;
    params.srcGrid[3] = pass.srcGridHeight;
    params.regionTop = pass.regionTop;
    params.rowCount = rowCount;
    params.mode = int(pass.mode);
    params.nearest = pass.nearest ? 1 : 0;
    std::memcpy(params.defaultPixel, pass.defaultPixel, sizeof(params.defaultPixel));
    std::memcpy(params.clearPixel, pass.clearPixel, sizeof(params.clearPixel));
    std::memcpy(mapped, &params, sizeof(params));
    if (!pass.srcTiles.isEmpty())
        std::memcpy(mapped + srcOffset,
                    pass.srcTiles.constData(),
                    size_t(pass.srcTiles.size()) * sizeof(VkDeviceAddress));
    std::memcpy(mapped + dstOffset, pass.dstTiles.constData(), size_t(pass.dstTiles.size()) * sizeof(VkDeviceAddress));
    std::memcpy(mapped + rowsOffset, pass.rows.constData(), size_t(pass.rows.size()) * sizeof(qint32));
    std::memcpy(mapped + spansOffset, pass.spans.constData(), size_t(pass.spans.size()) * sizeof(qint32));

    const VkDeviceAddress base = m_tables->deviceAddress();
    const PushConstants constants{base,
                                  base + srcOffset,
                                  base + dstOffset,
                                  base + rowsOffset,
                                  base + spansOffset,
                                  m_flag->deviceAddress()};
    const VkCommandBuffer buffer = commands.commandBuffer();
    m_context.vk().vkCmdFillBuffer(buffer, m_flag->handle(), 0, 16, 0);
    commands.computeBarrier();
    m_pipeline->dispatch(buffer, constants, quint32(pass.dstGridWidth), quint32(pass.dstGridHeight * 64));
    // Make the flag visible to the host after the submission's wait.
    commands.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
    return true;
}

bool KisGpuPerspectivePass::failed() const
{
    quint32 value = 0;
    std::memcpy(&value, m_flag->mapped(), sizeof(value));
    return value != 0;
}
