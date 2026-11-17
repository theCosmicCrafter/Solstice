/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuCoveragePass.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <cstring>

namespace
{
const quint32 ApplyCoverageRgba32f[] = {
#include "apply_coverage_rgba32f.spv.inc"
};

const quint32 ApplyCoverageRgba16f[] = {
#include "apply_coverage_rgba16f.spv.inc"
};

struct PushConstants {
    VkDeviceAddress tiles;
    VkDeviceAddress mask;
    quint32 tileCount;
    quint32 gridWidth;
    qint32 maskOrigin[2];
    qint32 maskSize[2];
    qint32 padding[2];
    qint32 clip[4];
};
static_assert(sizeof(PushConstants) == 64, "must match the shader push constant block");

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

KisGpuCoveragePass::KisGpuCoveragePass(KisGpuContext &context)
    : m_context(context)
{
}

KisGpuCoveragePass::~KisGpuCoveragePass() = default;

std::unique_ptr<KisGpuCoveragePass>
KisGpuCoveragePass::create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage)
{
    std::unique_ptr<KisGpuCoveragePass> pass(new KisGpuCoveragePass(context));
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    pass->m_pipeline = context.sharedComputePipeline(f16 ? ApplyCoverageRgba16f : ApplyCoverageRgba32f,
                                                     f16 ? sizeof(ApplyCoverageRgba16f) : sizeof(ApplyCoverageRgba32f),
                                                     sizeof(PushConstants),
                                                     errorMessage);
    return pass->m_pipeline ? std::move(pass) : nullptr;
}

bool KisGpuCoveragePass::record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage)
{
    if (pass.tiles.isEmpty() || pass.clip.isEmpty()) {
        return true;
    }
    const qint64 maskBytes = qint64(pass.maskBounds.width()) * pass.maskBounds.height();
    if (pass.gridWidth <= 0 || !pass.mask || pass.maskBounds.isEmpty() || maskBytes > MaxMaskBytes) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid or oversized coverage mask");
        }
        return false;
    }

    // One table: tile addresses, then the coverage bytes padded to words.
    const VkDeviceSize tileBytes = VkDeviceSize(pass.tiles.size()) * sizeof(VkDeviceAddress);
    const VkDeviceSize maskOffset = alignUp(tileBytes, 16);
    const VkDeviceSize paddedMaskBytes = alignUp(VkDeviceSize(maskBytes), 4);
    const VkDeviceSize bytes = maskOffset + paddedMaskBytes;
    if (!m_tables || m_tables->size() < bytes) {
        m_tables = KisGpuBuffer::create(m_context, bytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables) {
            return false;
        }
    }
    quint8 *table = static_cast<quint8 *>(m_tables->mapped());
    std::memcpy(table, pass.tiles.constData(), size_t(tileBytes));
    std::memset(table + maskOffset + paddedMaskBytes - 4, 0, 4);
    std::memcpy(table + maskOffset, pass.mask, size_t(maskBytes));

    PushConstants constants{};
    constants.tiles = m_tables->deviceAddress();
    constants.mask = m_tables->deviceAddress() + maskOffset;
    constants.tileCount = quint32(pass.tiles.size());
    constants.gridWidth = quint32(pass.gridWidth);
    constants.maskOrigin[0] = pass.maskBounds.x();
    constants.maskOrigin[1] = pass.maskBounds.y();
    constants.maskSize[0] = pass.maskBounds.width();
    constants.maskSize[1] = pass.maskBounds.height();
    constants.clip[0] = pass.clip.left();
    constants.clip[1] = pass.clip.top();
    constants.clip[2] = pass.clip.right() + 1;
    constants.clip[3] = pass.clip.bottom() + 1;
    m_pipeline->dispatch(commands.commandBuffer(), constants, quint32(pass.tiles.size()));
    return true;
}
